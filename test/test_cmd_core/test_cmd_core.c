/* Host tests of components/rmx_codec/cmd_core.c (wiki Mower-Protocol §5).
 * Run: pio test -e native */
#include <stdio.h>
#include <string.h>
#include <unity.h>
#include "cmd_core.h"

#define S 1000000LL

static cmd_mem_t mem;

void setUp(void) { memset(&mem, 0, sizeof(mem)); }
void tearDown(void) { cmd_mem_clear(&mem); }

static void test_id_rules(void)
{
    char id65[66];
    memset(id65, 'a', 65);
    id65[65] = '\0';
    TEST_ASSERT_TRUE(cmd_id_valid("a"));
    TEST_ASSERT_TRUE(cmd_id_valid("Az09_-"));
    TEST_ASSERT_TRUE(cmd_id_valid(id65 + 1));   /* 64 chars */
    TEST_ASSERT_FALSE(cmd_id_valid(id65));
    TEST_ASSERT_FALSE(cmd_id_valid(""));
    TEST_ASSERT_FALSE(cmd_id_valid(NULL));
    TEST_ASSERT_FALSE(cmd_id_valid("a b"));
    TEST_ASSERT_FALSE(cmd_id_valid("a.b"));
    TEST_ASSERT_FALSE(cmd_id_valid("a/b"));
}

static void test_name_rules(void)
{
    TEST_ASSERT_TRUE(cmd_name_valid("ping"));
    TEST_ASSERT_TRUE(cmd_name_valid("12345678901234567890123456789012"));
    TEST_ASSERT_FALSE(cmd_name_valid("123456789012345678901234567890123"));
    TEST_ASSERT_FALSE(cmd_name_valid(""));
    TEST_ASSERT_FALSE(cmd_name_valid(NULL));
}

static void test_repeat_returns_last_ack(void)
{
    TEST_ASSERT_NULL(cmd_mem_find(&mem, "x", 0));
    TEST_ASSERT_TRUE(cmd_mem_store(&mem, "x", "{\"state\":\"accepted\"}", true, 0));
    TEST_ASSERT_TRUE(cmd_mem_store(&mem, "x", "{\"state\":\"done\"}", false, 5 * S));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"done\"}", cmd_mem_find(&mem, "x", 6 * S));
    TEST_ASSERT_NULL(cmd_mem_find(&mem, "y", 6 * S));
    TEST_ASSERT_NULL(cmd_mem_find(&mem, NULL, 6 * S));
}

static void test_store_rejects_bad_id(void)
{
    TEST_ASSERT_FALSE(cmd_mem_store(&mem, "", "{}", false, 0));
    TEST_ASSERT_FALSE(cmd_mem_store(&mem, "a b", "{}", false, 0));
    TEST_ASSERT_FALSE(cmd_mem_store(&mem, "a", NULL, false, 0));
}

static void fill(int n, bool open, int64_t t0)
{
    for (int i = 0; i < n; i++) {
        char id[16];
        snprintf(id, sizeof(id), "id%d", i);
        TEST_ASSERT_TRUE(cmd_mem_store(&mem, id, id, open, t0 + i * S));
    }
}

static void test_evicts_oldest_finished_first(void)
{
    fill(CMD_MEM_IDS, false, 0);
    mem.slot[0].open = true;   /* id0 is the oldest but still runs */
    TEST_ASSERT_TRUE(cmd_mem_store(&mem, "new", "n", false, 30 * S));
    TEST_ASSERT_NOT_NULL(cmd_mem_find(&mem, "id0", 31 * S));
    TEST_ASSERT_NULL(cmd_mem_find(&mem, "id1", 31 * S));
    TEST_ASSERT_EQUAL_STRING("n", cmd_mem_find(&mem, "new", 31 * S));
}

static void test_evicts_oldest_when_all_open(void)
{
    fill(CMD_MEM_IDS, true, 0);
    TEST_ASSERT_TRUE(cmd_mem_store(&mem, "new", "n", true, 30 * S));
    TEST_ASSERT_NULL(cmd_mem_find(&mem, "id0", 31 * S));
    TEST_ASSERT_NOT_NULL(cmd_mem_find(&mem, "id1", 31 * S));
}

static void test_expired_finished_slot_goes_first(void)
{
    fill(CMD_MEM_IDS, false, 0);
    /* id5 refreshed (a repeat) so it is not the oldest, then expires: after
     * 3 minutes the first expired slot in table order is taken, here id0. */
    cmd_mem_find(&mem, "id5", 2 * S);
    TEST_ASSERT_TRUE(cmd_mem_store(&mem, "new", "n", false, 2 * S + CMD_MEM_US + 1));
    TEST_ASSERT_NULL(cmd_mem_find(&mem, "id0", 0));
    TEST_ASSERT_NOT_NULL(cmd_mem_find(&mem, "id5", 0));
}

static void test_find_refreshes_time(void)
{
    fill(CMD_MEM_IDS, false, 0);
    cmd_mem_find(&mem, "id0", 100 * S);   /* id0 repeated: now the newest */
    TEST_ASSERT_TRUE(cmd_mem_store(&mem, "new", "n", false, 101 * S));
    TEST_ASSERT_NOT_NULL(cmd_mem_find(&mem, "id0", 102 * S));
    TEST_ASSERT_NULL(cmd_mem_find(&mem, "id1", 102 * S));
}

static void test_open_slot_never_expires(void)
{
    fill(CMD_MEM_IDS, true, 0);
    mem.slot[3].open = false;
    mem.slot[3].t_us = 10 * S;   /* finished, not the oldest */
    TEST_ASSERT_TRUE(cmd_mem_store(&mem, "new", "n", false, 10 * S + CMD_MEM_US + 1));
    TEST_ASSERT_NULL(cmd_mem_find(&mem, "id3", 0));
    TEST_ASSERT_NOT_NULL(cmd_mem_find(&mem, "id0", 0));
}

static void test_expires(void)
{
    const char *msg = NULL;
    TEST_ASSERT_NULL(cmd_check_expires(CMD_EXPIRES_ABSENT, 0, -1, &msg));
    TEST_ASSERT_EQUAL_STRING(CMD_E_BAD_REQUEST, cmd_check_expires(CMD_EXPIRES_OTHER, 0, 1e9, &msg));
    TEST_ASSERT_NOT_NULL(msg);
    TEST_ASSERT_EQUAL_STRING(CMD_E_NO_CLOCK, cmd_check_expires(CMD_EXPIRES_NUMBER, 2e9, -1, &msg));
    TEST_ASSERT_EQUAL_STRING(CMD_E_EXPIRED, cmd_check_expires(CMD_EXPIRES_NUMBER, 1000, 1000.5, &msg));
    TEST_ASSERT_NULL(cmd_check_expires(CMD_EXPIRES_NUMBER, 1000, 1000, &msg));   /* at the second: still valid */
    TEST_ASSERT_NULL(cmd_check_expires(CMD_EXPIRES_NUMBER, 1000, 999, &msg));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_id_rules);
    RUN_TEST(test_name_rules);
    RUN_TEST(test_repeat_returns_last_ack);
    RUN_TEST(test_store_rejects_bad_id);
    RUN_TEST(test_evicts_oldest_finished_first);
    RUN_TEST(test_evicts_oldest_when_all_open);
    RUN_TEST(test_expired_finished_slot_goes_first);
    RUN_TEST(test_find_refreshes_time);
    RUN_TEST(test_open_slot_never_expires);
    RUN_TEST(test_expires);
    return UNITY_END();
}
