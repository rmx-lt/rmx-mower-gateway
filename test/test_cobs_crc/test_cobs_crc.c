/* COBS reference vectors and CRC-16/CCITT-FALSE known answers. */
#include <string.h>
#include <unity.h>
#include "cobs.h"
#include "crc16.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t in[600], out[700], back[700];

static void check(const uint8_t *raw, size_t n, const uint8_t *enc, size_t en)
{
    size_t got = cobs_encode(raw, n, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(en, got);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(enc, out, en);
    for (size_t i = 0; i < got; i++) {
        TEST_ASSERT_NOT_EQUAL(0, out[i]);
    }
    size_t dn = 0;
    TEST_ASSERT_TRUE(cobs_decode(out, got, back, sizeof(back), &dn));
    TEST_ASSERT_EQUAL_size_t(n, dn);
    if (n) {
        TEST_ASSERT_EQUAL_UINT8_ARRAY(raw, back, n);
    }
}

/* The examples of the COBS paper / Wikipedia. */
static void test_cobs_small_vectors(void)
{
    check(NULL, 0, (const uint8_t[]){ 0x01 }, 1);
    check((const uint8_t[]){ 0x00 }, 1, (const uint8_t[]){ 0x01, 0x01 }, 2);
    check((const uint8_t[]){ 0x00, 0x00 }, 2, (const uint8_t[]){ 0x01, 0x01, 0x01 }, 3);
    check((const uint8_t[]){ 0x00, 0x11, 0x00 }, 3, (const uint8_t[]){ 0x01, 0x02, 0x11, 0x01 }, 4);
    check((const uint8_t[]){ 0x11, 0x22, 0x00, 0x33 }, 4, (const uint8_t[]){ 0x03, 0x11, 0x22, 0x02, 0x33 }, 5);
    check((const uint8_t[]){ 0x11, 0x22, 0x33, 0x44 }, 4, (const uint8_t[]){ 0x05, 0x11, 0x22, 0x33, 0x44 }, 5);
    check((const uint8_t[]){ 0x11, 0x00, 0x00, 0x00 }, 4, (const uint8_t[]){ 0x02, 0x11, 0x01, 0x01, 0x01 }, 5);
}

static void test_cobs_254_boundaries(void)
{
    /* 01..FE (254 bytes) -> FF 01..FE: no extra code byte after a full block */
    for (int i = 0; i < 254; i++) in[i] = (uint8_t)(i + 1);
    out[0] = 0;
    uint8_t enc[300];
    enc[0] = 0xFF;
    memcpy(enc + 1, in, 254);
    check(in, 254, enc, 255);

    /* 00 01..FE (255) -> 01 FF 01..FE */
    in[0] = 0;
    for (int i = 1; i < 255; i++) in[i] = (uint8_t)i;
    enc[0] = 0x01;
    enc[1] = 0xFF;
    memcpy(enc + 2, in + 1, 254);
    check(in, 255, enc, 256);

    /* 01..FF (255) -> FF 01..FE 02 FF */
    for (int i = 0; i < 255; i++) in[i] = (uint8_t)(i + 1);
    enc[0] = 0xFF;
    memcpy(enc + 1, in, 254);
    enc[255] = 0x02;
    enc[256] = 0xFF;
    check(in, 255, enc, 257);

    /* 02..FF 00 (255) -> FF 02..FF 01 01 */
    for (int i = 0; i < 254; i++) in[i] = (uint8_t)(i + 2);
    in[254] = 0;
    enc[0] = 0xFF;
    memcpy(enc + 1, in, 254);
    enc[255] = 0x01;
    enc[256] = 0x01;
    check(in, 255, enc, 257);
}

static void test_cobs_worst_case_size(void)
{
    memset(in, 0x55, sizeof(in));   /* no zeros: the most overhead */
    size_t n = cobs_encode(in, sizeof(in), out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(n <= COBS_MAX_ENCODED(sizeof(in)));
}

static void test_cobs_output_too_small(void)
{
    uint8_t raw[4] = { 1, 2, 3, 4 };
    TEST_ASSERT_EQUAL_size_t(0, cobs_encode(raw, 4, out, 4));   /* needs 5 */
    TEST_ASSERT_EQUAL_size_t(5, cobs_encode(raw, 4, out, 5));
    size_t dn;
    TEST_ASSERT_FALSE(cobs_decode((const uint8_t[]){ 0x05, 1, 2, 3, 4 }, 5, back, 3, &dn));
}

static void test_cobs_decode_rejects(void)
{
    size_t dn;
    TEST_ASSERT_FALSE(cobs_decode(out, 0, back, sizeof(back), &dn));                          /* empty */
    TEST_ASSERT_FALSE(cobs_decode((const uint8_t[]){ 0x00 }, 1, back, sizeof(back), &dn));    /* zero code */
    TEST_ASSERT_FALSE(cobs_decode((const uint8_t[]){ 0x03, 0x11 }, 2, back, sizeof(back), &dn));         /* runs past end */
    TEST_ASSERT_FALSE(cobs_decode((const uint8_t[]){ 0x03, 0x11, 0x00 }, 3, back, sizeof(back), &dn));   /* zero inside */
}

static void test_cobs_incremental_equals_bulk(void)
{
    for (size_t i = 0; i < sizeof(in); i++) in[i] = (uint8_t)(i * 7 % 5);   /* zeros spread around */
    size_t bulk = cobs_encode(in, sizeof(in), back, sizeof(back));
    cobs_enc_t e;
    cobs_enc_begin(&e, out, sizeof(out));
    for (size_t i = 0; i < sizeof(in); i++) cobs_enc_put(&e, in[i]);
    TEST_ASSERT_EQUAL_size_t(bulk, cobs_enc_end(&e));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(back, out, bulk);
}

/* CRC-16/CCITT-FALSE: catalogue check value and a few known answers. */
static void test_crc16_known_answers(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16_ccitt_false((const uint8_t *)"123456789", 9));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, crc16_ccitt_false(NULL, 0));
    TEST_ASSERT_EQUAL_HEX16(0xB915, crc16_ccitt_false((const uint8_t *)"A", 1));
    TEST_ASSERT_EQUAL_HEX16(0xE1F0, crc16_ccitt_false((const uint8_t[]){ 0x00 }, 1));
}

static void test_crc16_incremental(void)
{
    const uint8_t *s = (const uint8_t *)"123456789";
    uint16_t c = crc16_update(CRC16_INIT, s, 4);
    c = crc16_update(c, s + 4, 5);
    TEST_ASSERT_EQUAL_HEX16(0x29B1, c);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cobs_small_vectors);
    RUN_TEST(test_cobs_254_boundaries);
    RUN_TEST(test_cobs_worst_case_size);
    RUN_TEST(test_cobs_output_too_small);
    RUN_TEST(test_cobs_decode_rejects);
    RUN_TEST(test_cobs_incremental_equals_bulk);
    RUN_TEST(test_crc16_known_answers);
    RUN_TEST(test_crc16_incremental);
    return UNITY_END();
}
