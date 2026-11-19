/* Reliable frames on the board link (wiki Mower-Protocol §14.2), injected clock. */
#include <unity.h>
#include "bl_reliable.h"

void setUp(void) {}
void tearDown(void) {}

static void test_numbers_wrap_and_skip_zero(void)
{
    bl_rel_tx_t t;
    bl_rel_tx_init(&t);
    for (int i = 1; i <= 255; i++) {
        uint8_t s = bl_rel_tx_start(&t, 0);
        TEST_ASSERT_EQUAL_UINT8(i, s);
        TEST_ASSERT_TRUE(bl_rel_tx_ack(&t, s));
    }
    TEST_ASSERT_EQUAL_UINT8(1, bl_rel_tx_start(&t, 0));   /* 255 -> 1, never 0 */
}

static void test_one_in_flight(void)
{
    bl_rel_tx_t t;
    bl_rel_tx_init(&t);
    TEST_ASSERT_EQUAL_UINT8(1, bl_rel_tx_start(&t, 0));
    TEST_ASSERT_TRUE(bl_rel_tx_busy(&t));
    TEST_ASSERT_EQUAL_UINT8(0, bl_rel_tx_start(&t, 10));
    TEST_ASSERT_FALSE(bl_rel_tx_ack(&t, 2));   /* not ours */
    TEST_ASSERT_TRUE(bl_rel_tx_busy(&t));
    TEST_ASSERT_TRUE(bl_rel_tx_ack(&t, 1));
    TEST_ASSERT_FALSE(bl_rel_tx_ack(&t, 1));   /* a late repeat ACK changes nothing */
    TEST_ASSERT_EQUAL_UINT8(2, bl_rel_tx_start(&t, 20));
}

static void test_resend_5_times_then_drop(void)
{
    bl_rel_tx_t t;
    bl_rel_tx_init(&t);
    uint32_t now = 1000;
    bl_rel_tx_start(&t, now);
    TEST_ASSERT_EQUAL(BL_REL_NONE, bl_rel_tx_poll(&t, now + 199));
    int resends = 0;
    for (int i = 0; i < 5; i++) {
        now += 200;
        TEST_ASSERT_EQUAL(BL_REL_NONE, bl_rel_tx_poll(&t, now - 1));
        TEST_ASSERT_EQUAL(BL_REL_RESEND, bl_rel_tx_poll(&t, now));
        TEST_ASSERT_EQUAL(BL_REL_NONE, bl_rel_tx_poll(&t, now));   /* timer restarted */
        resends++;
    }
    TEST_ASSERT_EQUAL(5, resends);
    TEST_ASSERT_EQUAL(BL_REL_NONE, bl_rel_tx_poll(&t, now + 199));
    TEST_ASSERT_EQUAL(BL_REL_DROPPED, bl_rel_tx_poll(&t, now + 200));
    TEST_ASSERT_EQUAL_UINT32(1, t.dropped);
    TEST_ASSERT_FALSE(bl_rel_tx_busy(&t));
    TEST_ASSERT_EQUAL(BL_REL_NONE, bl_rel_tx_poll(&t, now + 10000));
    TEST_ASSERT_EQUAL_UINT8(2, bl_rel_tx_start(&t, now));   /* the next frame gets the next number */
}

static void test_ack_after_resend(void)
{
    bl_rel_tx_t t;
    bl_rel_tx_init(&t);
    bl_rel_tx_start(&t, 0);
    TEST_ASSERT_EQUAL(BL_REL_RESEND, bl_rel_tx_poll(&t, 250));
    TEST_ASSERT_TRUE(bl_rel_tx_ack(&t, 1));
    TEST_ASSERT_EQUAL(BL_REL_NONE, bl_rel_tx_poll(&t, 5000));
    TEST_ASSERT_EQUAL_UINT32(0, t.dropped);
}

static void test_clock_wraps(void)
{
    bl_rel_tx_t t;
    bl_rel_tx_init(&t);
    bl_rel_tx_start(&t, 0xFFFFFFA0u);
    TEST_ASSERT_EQUAL(BL_REL_NONE, bl_rel_tx_poll(&t, 0xFFFFFFFFu));     /* 95 ms */
    TEST_ASSERT_EQUAL(BL_REL_NONE, bl_rel_tx_poll(&t, 0x00000067u));     /* 199 ms, after the wrap */
    TEST_ASSERT_EQUAL(BL_REL_RESEND, bl_rel_tx_poll(&t, 0x00000068u));   /* 200 ms */
}

static void test_abort(void)
{
    bl_rel_tx_t t;
    bl_rel_tx_init(&t);
    bl_rel_tx_start(&t, 0);
    bl_rel_tx_abort(&t);
    TEST_ASSERT_FALSE(bl_rel_tx_busy(&t));
    TEST_ASSERT_EQUAL_UINT32(0, t.dropped);
    TEST_ASSERT_EQUAL(BL_REL_NONE, bl_rel_tx_poll(&t, 1000));
}

static void test_rx_repeats(void)
{
    bl_rel_rx_t r;
    bl_rel_rx_init(&r);
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 1));
    TEST_ASSERT_FALSE(bl_rel_rx_accept(&r, 1));   /* resend after a lost ACK */
    TEST_ASSERT_FALSE(bl_rel_rx_accept(&r, 1));
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 2));
    TEST_ASSERT_FALSE(bl_rel_rx_accept(&r, 0));   /* 0 is not a reliable number */
}

static void test_rx_out_of_order_is_new(void)
{
    bl_rel_rx_t r;
    bl_rel_rx_init(&r);
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 5));
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 7));   /* 6 was dropped by the sender: no gap check */
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 5));   /* only the last number counts as a repeat */
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 255));
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 1));   /* wrap */
}

static void test_rx_reset_after_peer_restart(void)
{
    bl_rel_rx_t r;
    bl_rel_rx_init(&r);
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 1));
    /* the peer restarts and numbers from 1 again: without a reset its first frame looks like a repeat */
    TEST_ASSERT_FALSE(bl_rel_rx_accept(&r, 1));
    bl_rel_rx_reset(&r);
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 1));
}

/* Peer restart detection with the heartbeat's boot byte (Mower-Protocol §14.4). */
static void test_boot_first_heartbeat_only_records(void)
{
    bl_rel_rx_t r;
    bl_rel_rx_init(&r);
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 1));
    TEST_ASSERT_FALSE(bl_rel_rx_peer_boot(&r, 42));   /* first heartbeat: nothing to compare with */
    TEST_ASSERT_FALSE(bl_rel_rx_accept(&r, 1));       /* last number kept: still a repeat */
}

static void test_boot_change_resets(void)
{
    bl_rel_rx_t r;
    bl_rel_rx_init(&r);
    bl_rel_rx_peer_boot(&r, 7);
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 1));
    TEST_ASSERT_TRUE(bl_rel_rx_peer_boot(&r, 8));     /* the peer restarted */
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 1));        /* its new frame 1 is passed on */
    bl_rel_rx_peer_boot(&r, 255);
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 1));
    TEST_ASSERT_TRUE(bl_rel_rx_peer_boot(&r, 0));     /* the byte wraps like the counter */
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 1));
}

static void test_boot_same_value_keeps(void)
{
    bl_rel_rx_t r;
    bl_rel_rx_init(&r);
    bl_rel_rx_peer_boot(&r, 7);
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 3));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_FALSE(bl_rel_rx_peer_boot(&r, 7));
    }
    TEST_ASSERT_FALSE(bl_rel_rx_accept(&r, 3));       /* a resend is still a repeat */
}

static void test_boot_lost_heartbeats_keep(void)
{
    /* heartbeats 2..4 lost (fewer than 3 s: the link stays up): no call, nothing changes */
    bl_rel_rx_t r;
    bl_rel_rx_init(&r);
    bl_rel_rx_peer_boot(&r, 9);
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, 5));
    TEST_ASSERT_FALSE(bl_rel_rx_accept(&r, 5));
    TEST_ASSERT_FALSE(bl_rel_rx_peer_boot(&r, 9));    /* heartbeat 5 arrives, same boot */
    TEST_ASSERT_FALSE(bl_rel_rx_accept(&r, 5));
}

static void test_boot_quick_restart_end_to_end(void)
{
    /* The peer restarts faster than the 3 s timeout and sends a heartbeat before
     * its first reliable frame (the #62 contract): frame 1 of the new boot gets through. */
    bl_rel_tx_t peer;
    bl_rel_rx_t r;
    bl_rel_tx_init(&peer);
    bl_rel_rx_init(&r);
    bl_rel_rx_peer_boot(&r, 20);
    uint8_t s = bl_rel_tx_start(&peer, 0);
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, s));
    bl_rel_tx_init(&peer);                            /* restart: numbers from 1 again */
    TEST_ASSERT_TRUE(bl_rel_rx_peer_boot(&r, 21));    /* heartbeat first */
    s = bl_rel_tx_start(&peer, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, s);
    TEST_ASSERT_TRUE(bl_rel_rx_accept(&r, s));
}

/* Both ends together over a link that loses frames. */
static void test_lossy_link_end_to_end(void)
{
    bl_rel_tx_t t;
    bl_rel_rx_t r;
    bl_rel_tx_init(&t);
    bl_rel_rx_init(&r);
    int delivered = 0;
    uint32_t now = 0;
    uint32_t rng = 12345;
    for (int msg = 0; msg < 500; msg++) {
        uint8_t seq = bl_rel_tx_start(&t, now);
        TEST_ASSERT_NOT_EQUAL(0, seq);
        bool send = true;
        for (;;) {
            if (send) {
                rng = rng * 1103515245u + 12345u;
                bool frame_lost = (rng >> 16) % 4 == 0;   /* 25 % loss each way */
                rng = rng * 1103515245u + 12345u;
                bool ack_lost = (rng >> 16) % 4 == 0;
                if (!frame_lost) {
                    delivered += bl_rel_rx_accept(&r, seq);
                    if (!ack_lost && bl_rel_tx_ack(&t, seq)) {
                        break;
                    }
                }
            }
            now += 50;
            bl_rel_action_t a = bl_rel_tx_poll(&t, now);
            if (a == BL_REL_DROPPED) {
                break;
            }
            send = a == BL_REL_RESEND;
        }
        now += 10;
    }
    /* every message reached the other side at most once; drops are counted */
    TEST_ASSERT_TRUE(delivered <= 500);
    TEST_ASSERT_TRUE(delivered >= 500 - (int)t.dropped);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_numbers_wrap_and_skip_zero);
    RUN_TEST(test_one_in_flight);
    RUN_TEST(test_resend_5_times_then_drop);
    RUN_TEST(test_ack_after_resend);
    RUN_TEST(test_clock_wraps);
    RUN_TEST(test_abort);
    RUN_TEST(test_rx_repeats);
    RUN_TEST(test_rx_out_of_order_is_new);
    RUN_TEST(test_rx_reset_after_peer_restart);
    RUN_TEST(test_boot_first_heartbeat_only_records);
    RUN_TEST(test_boot_change_resets);
    RUN_TEST(test_boot_same_value_keeps);
    RUN_TEST(test_boot_lost_heartbeats_keep);
    RUN_TEST(test_boot_quick_restart_end_to_end);
    RUN_TEST(test_lossy_link_end_to_end);
    return UNITY_END();
}
