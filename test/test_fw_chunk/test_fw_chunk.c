/* Host tests of components/rmx_codec/fw_chunk.c (wiki Mower-Protocol §12.2).
 * Run: pio test -e native */
#include <string.h>
#include <unity.h>
#include "fw_chunk.h"

static const uint8_t HASH[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static uint8_t frame[FW_CHUNK_HDR_LEN + FW_CHUNK_MAX + 1];

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF; }

/* A valid frame with chunk_len payload bytes (0xA5); returns its length. */
static size_t make(uint16_t chunk_len)
{
    memset(frame, 0, sizeof(frame));
    memcpy(frame, "RMFW", 4);
    frame[4] = 1;
    frame[5] = 1;
    put16(frame + 6, 64);
    put32(frame + 8, 0x01020304);
    put32(frame + 12, 178);
    put32(frame + 16, 729088);
    put16(frame + 20, chunk_len);
    memcpy(frame + 24, HASH, 8);
    for (int i = 0; i < 32; i++) frame[32 + i] = (uint8_t)(0xC0 + i);
    memset(frame + 64, 0xA5, chunk_len);
    return FW_CHUNK_HDR_LEN + chunk_len;
}

static bool parse(size_t len, fw_chunk_hdr_t *h, const uint8_t **p)
{
    return fw_chunk_parse(frame, len, HASH, h, p);
}

void setUp(void) {}
void tearDown(void) {}

static void test_valid_full_chunk(void)
{
    fw_chunk_hdr_t h;
    const uint8_t *p = NULL;
    size_t len = make(4096);
    TEST_ASSERT_TRUE(parse(len, &h, &p));
    TEST_ASSERT_EQUAL_UINT32(0x01020304, h.chunk_index);   /* little-endian */
    TEST_ASSERT_EQUAL_UINT32(178, h.chunk_count);
    TEST_ASSERT_EQUAL_UINT32(729088, h.image_size);
    TEST_ASSERT_EQUAL_UINT16(4096, h.chunk_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(HASH, h.request_id_hash, 8);
    TEST_ASSERT_EQUAL_HEX8(0xC0, h.sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0xDF, h.sha256[31]);
    TEST_ASSERT_EQUAL_PTR(frame + 64, p);
}

static void test_empty_chunk(void)
{
    fw_chunk_hdr_t h;
    const uint8_t *p;
    TEST_ASSERT_TRUE(parse(make(0), &h, &p));   /* an empty file is one chunk of length 0 */
    TEST_ASSERT_EQUAL_UINT16(0, h.chunk_len);
}

static void test_short_or_null(void)
{
    fw_chunk_hdr_t h;
    const uint8_t *p;
    make(0);
    TEST_ASSERT_FALSE(parse(63, &h, &p));
    TEST_ASSERT_FALSE(fw_chunk_parse(NULL, 64, HASH, &h, &p));
}

static void test_fixed_fields(void)
{
    fw_chunk_hdr_t h;
    const uint8_t *p;
    struct { size_t off; uint8_t val; } bad[] = {
        { 0, 'X' },     /* magic */
        { 4, 2 },       /* header version */
        { 5, 0 },       /* flags */
        { 6, 65 },      /* header length */
        { 7, 1 },       /* header length, high byte */
        { 22, 1 },      /* reserved */
        { 23, 1 },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        size_t len = make(16);
        frame[bad[i].off] = bad[i].val;
        TEST_ASSERT_FALSE_MESSAGE(parse(len, &h, &p), "a wrong fixed field was accepted");
    }
}

static void test_length_mismatch(void)
{
    fw_chunk_hdr_t h;
    const uint8_t *p;
    size_t len = make(100);
    TEST_ASSERT_FALSE(parse(len - 1, &h, &p));
    TEST_ASSERT_FALSE(parse(len + 1, &h, &p));
}

static void test_chunk_too_long(void)
{
    fw_chunk_hdr_t h;
    const uint8_t *p;
    make(0);
    put16(frame + 20, FW_CHUNK_MAX + 1);
    TEST_ASSERT_FALSE(parse(FW_CHUNK_HDR_LEN + FW_CHUNK_MAX + 1, &h, &p));
}

static void test_other_request(void)
{
    fw_chunk_hdr_t h;
    const uint8_t *p;
    size_t len = make(8);
    frame[31] ^= 0xFF;
    TEST_ASSERT_FALSE(parse(len, &h, &p));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_valid_full_chunk);
    RUN_TEST(test_empty_chunk);
    RUN_TEST(test_short_or_null);
    RUN_TEST(test_fixed_fields);
    RUN_TEST(test_length_mismatch);
    RUN_TEST(test_chunk_too_long);
    RUN_TEST(test_other_request);
    return UNITY_END();
}
