/* Host tests of components/rmx_codec/recovery.c (issue #79).
 * Run: pio test -e native */
#include <string.h>
#include <unity.h>
#include "recovery.h"

#define MIN (60 * 1000LL)

static const rec_cfg_t CFG = { .mqtt_down_ms = 5 * MIN, .link_down_ms = 30 * MIN };
static rec_state_t st;
static const char *why;

void setUp(void) { memset(&st, 0, sizeof(st)); why = NULL; }
void tearDown(void) {}

/* ppp, mqtt, link at minute m */
static rec_action_t at(double m, bool ppp, bool mqtt, bool link)
{
    return rec_step(&st, &CFG, (int64_t)(m * MIN), ppp, mqtt, link, false, &why);
}

static void test_all_up_does_nothing(void)
{
    for (int m = 0; m < 120; m++) {
        TEST_ASSERT_EQUAL(REC_NONE, at(m, true, true, true));
    }
}

static void test_mqtt_down_reconnects_every_period(void)
{
    TEST_ASSERT_EQUAL(REC_NONE, at(0, true, false, false));   /* first tick only starts the clocks */
    TEST_ASSERT_EQUAL(REC_NONE, at(1, true, false, false));   /* condition starts here */
    TEST_ASSERT_EQUAL(REC_NONE, at(5.9, true, false, false));
    TEST_ASSERT_EQUAL(REC_CELL_RECONNECT, at(6, true, false, false));
    TEST_ASSERT_EQUAL_STRING("MQTT down with PPP up", why);
    TEST_ASSERT_EQUAL(REC_NONE, at(10.9, true, false, false));
    TEST_ASSERT_EQUAL(REC_CELL_RECONNECT, at(11, true, false, false));
}

static void test_ppp_down_or_mqtt_up_resets_reconnect(void)
{
    at(0, true, false, true);
    at(1, true, false, true);
    at(4, false, false, true);   /* PPP dropped: cell redials by itself */
    TEST_ASSERT_EQUAL(REC_NONE, at(6, true, false, true));   /* clock started again at 6 */
    TEST_ASSERT_EQUAL(REC_NONE, at(10.9, true, false, true));
    TEST_ASSERT_EQUAL(REC_CELL_RECONNECT, at(11, true, false, true));
    at(12, true, true, true);
    TEST_ASSERT_EQUAL(REC_NONE, at(13, true, false, true));
    TEST_ASSERT_EQUAL(REC_NONE, at(17.9, true, false, true));
    TEST_ASSERT_EQUAL(REC_CELL_RECONNECT, at(18, true, false, true));
}

static void test_link_down_restarts(void)
{
    TEST_ASSERT_EQUAL(REC_NONE, at(0, false, false, false));
    TEST_ASSERT_EQUAL(REC_NONE, at(29.9, false, false, false));
    TEST_ASSERT_EQUAL(REC_RESTART, at(30, false, false, false));
    TEST_ASSERT_EQUAL_STRING("server link down too long", why);
}

static void test_link_up_resets_restart(void)
{
    at(0, true, true, false);
    at(20, true, true, true);
    TEST_ASSERT_EQUAL(REC_NONE, at(49.9, true, true, false));
    TEST_ASSERT_EQUAL(REC_RESTART, at(50, true, true, false));
}

static void test_restart_wins_over_reconnect(void)
{
    at(0, true, false, false);
    for (double m = 0.5; m < 30; m += 0.5) {
        at(m, true, false, false);
    }
    TEST_ASSERT_EQUAL(REC_RESTART, at(30, true, false, false));
}

static void test_ota_pauses_and_restarts_clocks(void)
{
    at(0, true, false, false);
    at(1, true, false, false);
    TEST_ASSERT_EQUAL(REC_NONE, rec_step(&st, &CFG, 40 * MIN, true, false, false, true, &why));
    TEST_ASSERT_EQUAL(REC_NONE, at(40.5, true, false, false));   /* the MQTT clock starts here */
    TEST_ASSERT_EQUAL(REC_CELL_RECONNECT, at(45.5, true, false, false));
    at(69.9, true, false, false);   /* (a reconnect is due again here) */
    TEST_ASSERT_EQUAL(REC_RESTART, at(70, true, false, false));
}

static void test_liveness(void)
{
    live_t t[3];
    memset(t, 0, sizeof(t));
    uint32_t c[3] = { 0, 0, 0 };
    TEST_ASSERT_EQUAL_INT(-1, live_check(t, c, 3, 0, 120000, false));
    c[0] = 1;
    c[2] = 1;
    TEST_ASSERT_EQUAL_INT(-1, live_check(t, c, 3, 119999, 120000, false));
    TEST_ASSERT_EQUAL_INT(1, live_check(t, c, 3, 120000, 120000, false));   /* task 1 never moved */
    c[1] = 7;
    TEST_ASSERT_EQUAL_INT(-1, live_check(t, c, 3, 150000, 120000, false));
    /* task 0 and 2 last moved at 119999 */
    TEST_ASSERT_EQUAL_INT(0, live_check(t, c, 3, 239999, 120000, false));
}

static void test_liveness_wraps_and_ota(void)
{
    live_t t[1];
    memset(t, 0, sizeof(t));
    uint32_t c[1] = { 0xFFFFFFFFu };
    live_check(t, c, 1, 0, 1000, false);
    c[0] = 0;   /* wrapped: still a change */
    TEST_ASSERT_EQUAL_INT(-1, live_check(t, c, 1, 999, 1000, false));
    TEST_ASSERT_EQUAL_INT(-1, live_check(t, c, 1, 5000, 1000, true));   /* OTA: alive */
    TEST_ASSERT_EQUAL_INT(-1, live_check(t, c, 1, 5999, 1000, false));
    TEST_ASSERT_EQUAL_INT(0, live_check(t, c, 1, 6000, 1000, false));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_all_up_does_nothing);
    RUN_TEST(test_mqtt_down_reconnects_every_period);
    RUN_TEST(test_ppp_down_or_mqtt_up_resets_reconnect);
    RUN_TEST(test_link_down_restarts);
    RUN_TEST(test_link_up_resets_restart);
    RUN_TEST(test_restart_wins_over_reconnect);
    RUN_TEST(test_ota_pauses_and_restarts_clocks);
    RUN_TEST(test_liveness);
    RUN_TEST(test_liveness_wraps_and_ota);
    return UNITY_END();
}
