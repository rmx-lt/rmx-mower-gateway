/* Offline queue of the board link (wiki Mower-Protocol §14.2): 60 s, 64 KB, oldest dropped. */
#include <string.h>
#include <unity.h>
#include "bl_queue.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t ring[BL_QUEUE_BYTES];
static uint8_t data[BL_QUEUE_BYTES];
static uint8_t out[BL_QUEUE_BYTES];

static bl_queue_t q;

static void fill(uint8_t tag, size_t n)
{
    memset(data, tag, n);
}

static void test_fifo_order(void)
{
    bl_queue_init(&q, ring, sizeof(ring), BL_QUEUE_MAX_AGE_MS);
    for (uint8_t i = 1; i <= 3; i++) {
        fill(i, i * 10);
        TEST_ASSERT_TRUE(bl_queue_push(&q, 0, data, i * 10));
    }
    TEST_ASSERT_EQUAL_UINT32(3, bl_queue_count(&q));
    for (uint8_t i = 1; i <= 3; i++) {
        size_t n;
        TEST_ASSERT_TRUE(bl_queue_peek(&q, 1, out, sizeof(out), &n));
        TEST_ASSERT_EQUAL_size_t(i * 10, n);
        TEST_ASSERT_EACH_EQUAL_UINT8(i, out, n);
        bl_queue_drop(&q);
    }
    size_t n;
    TEST_ASSERT_FALSE(bl_queue_peek(&q, 1, out, sizeof(out), &n));
    bl_queue_drop(&q);   /* on empty: harmless */
    TEST_ASSERT_EQUAL_UINT32(0, bl_queue_count(&q));
}

static void test_zero_length_entry(void)
{
    bl_queue_init(&q, ring, sizeof(ring), BL_QUEUE_MAX_AGE_MS);
    TEST_ASSERT_TRUE(bl_queue_push(&q, 0, NULL, 0));
    size_t n = 99;
    TEST_ASSERT_TRUE(bl_queue_peek(&q, 0, out, 0, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
}

static void test_overflow_drops_oldest(void)
{
    bl_queue_init(&q, ring, sizeof(ring), BL_QUEUE_MAX_AGE_MS);
    /* 4200-byte entries (a relayed chunk): 15 fit in 64 KB with overhead, the 16th pushes out the 1st */
    const size_t len = 4200;
    size_t fits = sizeof(ring) / (len + BL_QUEUE_ENTRY_OVERHEAD);
    TEST_ASSERT_EQUAL_size_t(15, fits);
    for (size_t i = 0; i < fits; i++) {
        fill((uint8_t)i, len);
        TEST_ASSERT_TRUE(bl_queue_push(&q, (uint32_t)i, data, len));
    }
    TEST_ASSERT_EQUAL_UINT32(0, q.dropped_full);
    fill(0xEE, len);
    TEST_ASSERT_TRUE(bl_queue_push(&q, 100, data, len));
    TEST_ASSERT_EQUAL_UINT32(1, q.dropped_full);
    TEST_ASSERT_EQUAL_UINT32(fits, bl_queue_count(&q));
    size_t n;
    TEST_ASSERT_TRUE(bl_queue_peek(&q, 100, out, sizeof(out), &n));
    TEST_ASSERT_EACH_EQUAL_UINT8(1, out, n);   /* entry 0 is gone */
    /* the newest one survived the wrap intact */
    for (size_t i = 1; i < fits; i++) bl_queue_drop(&q);
    TEST_ASSERT_TRUE(bl_queue_peek(&q, 100, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_size_t(len, n);
    TEST_ASSERT_EACH_EQUAL_UINT8(0xEE, out, n);
}

static void test_exact_fill_and_too_big(void)
{
    bl_queue_init(&q, ring, sizeof(ring), BL_QUEUE_MAX_AGE_MS);
    size_t max = sizeof(ring) - BL_QUEUE_ENTRY_OVERHEAD;
    fill(0x5A, max);
    TEST_ASSERT_TRUE(bl_queue_push(&q, 0, data, max));     /* exactly the whole buffer */
    TEST_ASSERT_EQUAL_size_t(sizeof(ring), q.used);
    TEST_ASSERT_FALSE(bl_queue_push(&q, 0, data, max + 1)); /* can never fit: refused, queue kept */
    TEST_ASSERT_EQUAL_UINT32(1, q.dropped_full);
    TEST_ASSERT_EQUAL_UINT32(1, bl_queue_count(&q));
    TEST_ASSERT_TRUE(bl_queue_push(&q, 0, data, 1));        /* pushes the big one out */
    TEST_ASSERT_EQUAL_UINT32(2, q.dropped_full);
    TEST_ASSERT_EQUAL_UINT32(1, bl_queue_count(&q));
}

static void test_peek_into_small_buffer(void)
{
    bl_queue_init(&q, ring, sizeof(ring), BL_QUEUE_MAX_AGE_MS);
    fill(7, 100);
    bl_queue_push(&q, 0, data, 100);
    size_t n = 0;
    TEST_ASSERT_FALSE(bl_queue_peek(&q, 0, out, 99, &n));
    TEST_ASSERT_EQUAL_size_t(100, n);
    TEST_ASSERT_EQUAL_UINT32(1, bl_queue_count(&q));   /* not removed */
}

static void test_aging(void)
{
    bl_queue_init(&q, ring, sizeof(ring), BL_QUEUE_MAX_AGE_MS);
    fill(1, 10);
    bl_queue_push(&q, 1000, data, 10);
    fill(2, 10);
    bl_queue_push(&q, 30000, data, 10);
    size_t n;
    TEST_ASSERT_TRUE(bl_queue_peek(&q, 60999, out, sizeof(out), &n));   /* 59.999 s old: kept */
    TEST_ASSERT_EQUAL_HEX8(1, out[0]);
    TEST_ASSERT_TRUE(bl_queue_peek(&q, 61000, out, sizeof(out), &n));   /* 60 s: dropped */
    TEST_ASSERT_EQUAL_HEX8(2, out[0]);
    TEST_ASSERT_EQUAL_UINT32(1, q.dropped_age);
    bl_queue_expire(&q, 90000);
    TEST_ASSERT_EQUAL_UINT32(0, bl_queue_count(&q));
    TEST_ASSERT_EQUAL_UINT32(2, q.dropped_age);
    /* push also expires */
    bl_queue_push(&q, 100000, data, 10);
    bl_queue_push(&q, 170000, data, 10);
    TEST_ASSERT_EQUAL_UINT32(1, bl_queue_count(&q));
    TEST_ASSERT_EQUAL_UINT32(3, q.dropped_age);
}

static void test_aging_across_clock_wrap(void)
{
    bl_queue_init(&q, ring, sizeof(ring), BL_QUEUE_MAX_AGE_MS);
    bl_queue_push(&q, 0xFFFFF000u, data, 10);
    bl_queue_expire(&q, 0x00001000u);   /* 8 s later */
    TEST_ASSERT_EQUAL_UINT32(1, bl_queue_count(&q));
    bl_queue_expire(&q, 0xFFFFF000u + 60000u);
    TEST_ASSERT_EQUAL_UINT32(0, bl_queue_count(&q));
}

static void test_many_small_entries_wrap(void)
{
    uint8_t small[100];
    bl_queue_init(&q, small, sizeof(small), BL_QUEUE_MAX_AGE_MS);
    uint32_t next_out = 0, pushed = 0;
    for (uint32_t i = 0; i < 1000; i++) {
        uint8_t v[5];
        memset(v, (uint8_t)i, sizeof(v));
        bl_queue_push(&q, i, v, (i % 5) + 1);   /* sizes 1..5: entries straddle the ring end */
        pushed++;
        if (i % 3 == 0) {
            size_t n;
            if (bl_queue_peek(&q, i, out, sizeof(out), &n)) {
                TEST_ASSERT_TRUE(n >= 1 && n <= 5);
                for (size_t k = 1; k < n; k++) TEST_ASSERT_EQUAL_HEX8(out[0], out[k]);
                TEST_ASSERT_TRUE((uint8_t)(out[0] - (uint8_t)next_out) < 128);   /* never goes back in time */
                next_out = out[0];
                bl_queue_drop(&q);
            }
        }
        TEST_ASSERT_TRUE(q.used <= sizeof(small));
    }
    TEST_ASSERT_TRUE(q.dropped_full > 0);
    (void)pushed;
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fifo_order);
    RUN_TEST(test_zero_length_entry);
    RUN_TEST(test_overflow_drops_oldest);
    RUN_TEST(test_exact_fill_and_too_big);
    RUN_TEST(test_peek_into_small_buffer);
    RUN_TEST(test_aging);
    RUN_TEST(test_aging_across_clock_wrap);
    RUN_TEST(test_many_small_entries_wrap);
    return UNITY_END();
}
