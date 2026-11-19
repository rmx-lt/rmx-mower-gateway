/* Board link frames and bodies (wiki Mower-Protocol §14.1, §14.2, §14.4). */
#include <string.h>
#include <unity.h>
#include "bl_frame.h"
#include "crc16.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t body[BL_FRAME_MAX + 8];
static uint8_t wire[BL_WIRE_MAX + 8];
static uint8_t buf[BL_FRAME_MAX];

/* Encodes, checks the trailing 0x00 and no other zero, decodes. */
static bl_err_t roundtrip(uint8_t type, size_t n, bl_frame_t *f)
{
    size_t w = bl_frame_encode(type, body, n, wire, sizeof(wire));
    TEST_ASSERT_TRUE(w >= 4);
    TEST_ASSERT_EQUAL_HEX8(0x00, wire[w - 1]);
    TEST_ASSERT_NULL(memchr(wire, 0, w - 1));
    return bl_frame_decode(wire, w - 1, buf, sizeof(buf), f);
}

static void test_known_frame_bytes(void)
{
    /* ACK of 5: raw 07 05 + CRC("\x07\x05") LE, then COBS and 0x00 */
    body[0] = 5;
    uint16_t crc = crc16_ccitt_false((const uint8_t[]){ 0x07, 0x05 }, 2);
    uint8_t raw[4] = { 0x07, 0x05, (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };
    uint8_t want[8];
    size_t wn = cobs_encode(raw, 4, want, sizeof(want));
    want[wn++] = 0;
    size_t w = bl_frame_encode(BL_ACK, body, 1, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_size_t(wn, w);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, wire, wn);
}

static void test_empty_body(void)
{
    bl_frame_t f;
    TEST_ASSERT_EQUAL(BL_OK, roundtrip(BL_HEARTBEAT, 0, &f));
    TEST_ASSERT_EQUAL(BL_HEARTBEAT, f.type);
    TEST_ASSERT_EQUAL_size_t(0, f.body_len);
}

static void test_zero_bytes_in_body(void)
{
    bl_frame_t f;
    memset(body, 0, 300);
    TEST_ASSERT_EQUAL(BL_OK, roundtrip(BL_CORRECTIONS, 300, &f));
    TEST_ASSERT_EQUAL_size_t(300, f.body_len);
    TEST_ASSERT_EACH_EQUAL_UINT8(0, f.body, 300);
}

static void test_max_frame(void)
{
    bl_frame_t f;
    for (size_t i = 0; i < BL_BODY_MAX; i++) body[i] = (uint8_t)(i * 31 + 1);
    TEST_ASSERT_EQUAL(BL_OK, roundtrip(BL_MESSAGE, BL_BODY_MAX, &f));
    TEST_ASSERT_EQUAL_size_t(BL_BODY_MAX, f.body_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(body, f.body, BL_BODY_MAX);
    /* one byte more does not encode */
    TEST_ASSERT_EQUAL_size_t(0, bl_frame_encode(BL_MESSAGE, body, BL_BODY_MAX + 1, wire, sizeof(wire)));
}

static void test_too_long_on_wire(void)
{
    /* A hand-made frame of BL_FRAME_MAX + 1 raw bytes with a valid CRC. */
    static uint8_t raw[BL_FRAME_MAX + 1];
    raw[0] = BL_CORRECTIONS;
    memset(raw + 1, 0x42, sizeof(raw) - 3);
    uint16_t crc = crc16_ccitt_false(raw, sizeof(raw) - 2);
    raw[sizeof(raw) - 2] = (uint8_t)crc;
    raw[sizeof(raw) - 1] = (uint8_t)(crc >> 8);
    size_t w = cobs_encode(raw, sizeof(raw), wire, sizeof(wire));
    TEST_ASSERT_TRUE(w > 0);
    bl_frame_t f;
    static uint8_t big[BL_FRAME_MAX + 64];
    TEST_ASSERT_EQUAL(BL_ERR_TOO_LONG, bl_frame_decode(wire, w, big, sizeof(big), &f));
    TEST_ASSERT_EQUAL(BL_ERR_TOO_LONG, bl_frame_decode(wire, w, buf, sizeof(buf), &f));
}

static void test_small_output_buffer(void)
{
    body[0] = 1;
    TEST_ASSERT_EQUAL_size_t(0, bl_frame_encode(BL_ACK, body, 1, wire, 4));   /* needs 5 + 0x00 */
    TEST_ASSERT_EQUAL_size_t(6, bl_frame_encode(BL_ACK, body, 1, wire, 6));
}

static void test_crc_error_every_bit(void)
{
    bl_frame_t f;
    memcpy(body, "hello", 5);
    size_t w = bl_frame_encode(BL_GGA, body, 5, wire, sizeof(wire));
    uint8_t raw[16];
    size_t rn;
    TEST_ASSERT_TRUE(cobs_decode(wire, w - 1, raw, sizeof(raw), &rn));
    for (size_t i = 0; i < rn * 8; i++) {
        uint8_t bad[16], enc[24];
        memcpy(bad, raw, rn);
        bad[i / 8] ^= (uint8_t)(1u << (i % 8));
        size_t en = cobs_encode(bad, rn, enc, sizeof(enc));
        TEST_ASSERT_EQUAL(BL_ERR_CRC, bl_frame_decode(enc, en, buf, sizeof(buf), &f));
    }
}

static void test_short_and_bad_cobs(void)
{
    bl_frame_t f;
    uint8_t enc[8];
    size_t en = cobs_encode((const uint8_t[]){ 0x07, 0x01 }, 2, enc, sizeof(enc));
    TEST_ASSERT_EQUAL(BL_ERR_SHORT, bl_frame_decode(enc, en, buf, sizeof(buf), &f));
    TEST_ASSERT_EQUAL(BL_ERR_COBS, bl_frame_decode((const uint8_t[]){ 0x05, 0x01 }, 2, buf, sizeof(buf), &f));
}

static void test_unknown_type(void)
{
    bl_frame_t f;
    size_t w = bl_frame_encode(0x08, body, 3, wire, sizeof(wire));
    TEST_ASSERT_EQUAL(BL_ERR_TYPE, bl_frame_decode(wire, w - 1, buf, sizeof(buf), &f));
    w = bl_frame_encode(0x00, body, 3, wire, sizeof(wire));
    TEST_ASSERT_EQUAL(BL_ERR_TYPE, bl_frame_decode(wire, w - 1, buf, sizeof(buf), &f));
}

static void test_deframer_stream(void)
{
    static uint8_t dbuf[BL_WIRE_MAX];
    bl_deframer_t d;
    bl_deframer_init(&d, dbuf, sizeof(dbuf));
    /* garbage, then two frames back to back, with an empty frame between */
    uint8_t stream[64];
    size_t n = 0;
    stream[n++] = 0x33;   /* noise before the first delimiter: a bad frame */
    stream[n++] = 0x00;
    stream[n++] = 0x00;   /* empty: skipped */
    body[0] = 1;
    n += bl_frame_encode(BL_ACK, body, 1, stream + n, sizeof(stream) - n);
    body[0] = 2;
    n += bl_frame_encode(BL_ACK, body, 1, stream + n, sizeof(stream) - n);
    int good = 0, bad = 0;
    uint8_t seqs[2];
    for (size_t i = 0; i < n; i++) {
        const uint8_t *fr;
        size_t fl;
        if (bl_deframer_push(&d, stream[i], &fr, &fl)) {
            bl_frame_t f;
            if (bl_frame_decode(fr, fl, buf, sizeof(buf), &f) == BL_OK) {
                seqs[good++] = f.body[0];
            } else {
                bad++;
            }
        }
    }
    TEST_ASSERT_EQUAL(2, good);
    TEST_ASSERT_EQUAL(1, bad);
    TEST_ASSERT_EQUAL_UINT8(1, seqs[0]);
    TEST_ASSERT_EQUAL_UINT8(2, seqs[1]);
}

static void test_deframer_overrun(void)
{
    uint8_t dbuf[4];
    bl_deframer_t d;
    bl_deframer_init(&d, dbuf, sizeof(dbuf));
    const uint8_t *fr;
    size_t fl;
    for (int i = 0; i < 10; i++) TEST_ASSERT_FALSE(bl_deframer_push(&d, 0x11, &fr, &fl));
    TEST_ASSERT_FALSE(bl_deframer_push(&d, 0x00, &fr, &fl));   /* the long one is discarded */
    TEST_ASSERT_EQUAL_UINT32(1, d.overruns);
    TEST_ASSERT_FALSE(bl_deframer_push(&d, 0x22, &fr, &fl));
    TEST_ASSERT_TRUE(bl_deframer_push(&d, 0x00, &fr, &fl));    /* and the next one is fine */
    TEST_ASSERT_EQUAL_size_t(1, fl);
    TEST_ASSERT_EQUAL_HEX8(0x22, fr[0]);
}

static void test_relay_roundtrip(void)
{
    bl_relay_t r = { .retain = true, .qos = 1, .seq = 9, .topic = "cmd/ack", .topic_len = 7,
                     .payload = (const uint8_t *)"{\"id\":\"c1\"}", .payload_len = 11 };
    size_t n = bl_relay_encode(&r, body, sizeof(body));
    TEST_ASSERT_EQUAL_size_t(3 + 7 + 11, n);
    TEST_ASSERT_EQUAL_HEX8(0x03, body[0]);   /* retain | QoS 1 << 1 */
    bl_relay_t p;
    TEST_ASSERT_EQUAL(BL_OK, bl_relay_parse(body, n, &p));
    TEST_ASSERT_TRUE(p.retain);
    TEST_ASSERT_EQUAL_UINT8(1, p.qos);
    TEST_ASSERT_EQUAL_UINT8(9, p.seq);
    TEST_ASSERT_EQUAL_MEMORY("cmd/ack", p.topic, 7);
    TEST_ASSERT_EQUAL_size_t(11, p.payload_len);
    /* empty payload, QoS 0 */
    bl_relay_t z = { .qos = 0, .seq = 0, .topic = "telemetry", .topic_len = 9 };
    n = bl_relay_encode(&z, body, sizeof(body));
    TEST_ASSERT_EQUAL(BL_OK, bl_relay_parse(body, n, &p));
    TEST_ASSERT_EQUAL_size_t(0, p.payload_len);
    TEST_ASSERT_FALSE(p.retain);
}

static void test_relay_rejects(void)
{
    bl_relay_t p;
    char t65[65];
    memset(t65, 'a', sizeof(t65));
    bl_relay_t r = { .qos = 1, .seq = 1, .topic = t65, .topic_len = 64 };
    TEST_ASSERT_TRUE(bl_relay_encode(&r, body, sizeof(body)) > 0);   /* 64 is the limit */
    r.topic_len = 65;
    TEST_ASSERT_EQUAL_size_t(0, bl_relay_encode(&r, body, sizeof(body)));
    r.topic_len = 0;
    TEST_ASSERT_EQUAL_size_t(0, bl_relay_encode(&r, body, sizeof(body)));
    /* QoS/seq mismatch, QoS 2, wildcard, space, truncated topic */
    const uint8_t qos1_seq0[] = { 0x02, 0, 1, 'a' };
    const uint8_t qos0_seq3[] = { 0x00, 3, 1, 'a' };
    const uint8_t qos2[] = { 0x04, 1, 1, 'a' };
    const uint8_t wild[] = { 0x02, 1, 2, 'a', '#' };
    const uint8_t space[] = { 0x02, 1, 2, 'a', ' ' };
    const uint8_t trunc[] = { 0x02, 1, 5, 'a', 'b' };
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_relay_parse(qos1_seq0, sizeof(qos1_seq0), &p));
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_relay_parse(qos0_seq3, sizeof(qos0_seq3), &p));
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_relay_parse(qos2, sizeof(qos2), &p));
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_relay_parse(wild, sizeof(wild), &p));
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_relay_parse(space, sizeof(space), &p));
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_relay_parse(trunc, sizeof(trunc), &p));
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_relay_parse(qos2, 2, &p));
    /* unknown flag bits are ignored */
    const uint8_t extra[] = { 0xF2, 1, 1, 'a' };
    TEST_ASSERT_EQUAL(BL_OK, bl_relay_parse(extra, sizeof(extra), &p));
}

static void test_subscribe(void)
{
    /* the version 0.1 list of §14.2 */
    const char *names[] = { "cmd", "drive", "path/chunk", "path/res", "fw/chunk", "fw/res" };
    bl_sub_entry_t in[6], out[8];
    for (int i = 0; i < 6; i++) {
        in[i] = (bl_sub_entry_t){ .qos = (uint8_t)(i == 1 ? 0 : 1), .topic = names[i], .topic_len = strlen(names[i]) };
    }
    size_t n = bl_subscribe_encode(42, in, 6, body, sizeof(body));
    TEST_ASSERT_TRUE(n > 0);
    uint8_t seq;
    size_t cnt;
    TEST_ASSERT_EQUAL(BL_OK, bl_subscribe_parse(body, n, &seq, out, 8, &cnt));
    TEST_ASSERT_EQUAL_UINT8(42, seq);
    TEST_ASSERT_EQUAL_size_t(6, cnt);
    TEST_ASSERT_EQUAL_UINT8(0, out[1].qos);
    TEST_ASSERT_EQUAL_MEMORY("path/chunk", out[2].topic, 10);
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_subscribe_parse(body, n, &seq, out, 5, &cnt));   /* too many */
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_subscribe_parse(body, n - 1, &seq, out, 8, &cnt)); /* truncated */
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_subscribe_parse((const uint8_t[]){ 7 }, 1, &seq, out, 8, &cnt)); /* no entry */
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_subscribe_parse((const uint8_t[]){ 0, 1, 1, 'a' }, 4, &seq, out, 8, &cnt));
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_subscribe_parse((const uint8_t[]){ 1, 1, 1, '+' }, 4, &seq, out, 8, &cnt));
    TEST_ASSERT_EQUAL_size_t(0, bl_subscribe_encode(0, in, 6, body, sizeof(body)));
}

static void test_ack(void)
{
    uint8_t seq;
    TEST_ASSERT_EQUAL(BL_OK, bl_ack_parse((const uint8_t[]){ 255 }, 1, &seq));
    TEST_ASSERT_EQUAL_UINT8(255, seq);
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_ack_parse((const uint8_t[]){ 0 }, 1, &seq));
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_ack_parse((const uint8_t[]){ 1, 2 }, 2, &seq));
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_ack_parse(body, 0, &seq));
}

static void test_heartbeats(void)
{
    bl_hb_gw_t h = { .version = 1, .flags = BL_HB_MQTT | BL_HB_CLOCK | BL_HB_SUBS_NEEDED,
                     .unix_s = 1794650000u, .ms = 250, .boot = 0xA7 };
    uint8_t b[10];
    TEST_ASSERT_EQUAL_size_t(0, bl_hb_gw_encode(&h, b, 8));
    TEST_ASSERT_EQUAL_size_t(9, bl_hb_gw_encode(&h, b, sizeof(b)));
    const uint8_t want[9] = { 1, 0x19, 0x90, 0x2F, 0xF8, 0x6A, 0xFA, 0x00, 0xA7 };   /* 1794650000 = 0x6AF82F90 LE; boot at offset 8 */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, b, 9);
    bl_hb_gw_t p;
    TEST_ASSERT_EQUAL(BL_OK, bl_hb_gw_parse(b, 9, &p));
    TEST_ASSERT_EQUAL_UINT32(1794650000u, p.unix_s);
    TEST_ASSERT_EQUAL_UINT16(250, p.ms);
    TEST_ASSERT_EQUAL_HEX8(0x19, p.flags);
    TEST_ASSERT_EQUAL_HEX8(0xA7, p.boot);
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_hb_gw_parse(b, 8, &p));   /* the pre-boot 8-byte body is too short */
    b[9] = 0x55;
    TEST_ASSERT_EQUAL(BL_OK, bl_hb_gw_parse(b, 10, &p));        /* extra bytes ignored */
    h.ms = 1000;
    TEST_ASSERT_EQUAL_size_t(0, bl_hb_gw_encode(&h, b, sizeof(b)));
    bl_hb_ap_t a = { .version = 1, .boot = 0x03 }, ap;
    TEST_ASSERT_EQUAL_size_t(3, bl_hb_ap_encode(&a, b, sizeof(b)));
    const uint8_t want_ap[3] = { 1, 0, 0x03 };                  /* reserved byte kept, boot at offset 2 */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want_ap, b, 3);
    b[1] = 0x77;                                                /* reserved is not checked */
    TEST_ASSERT_EQUAL(BL_OK, bl_hb_ap_parse(b, 3, &ap));
    TEST_ASSERT_EQUAL_UINT8(1, ap.version);
    TEST_ASSERT_EQUAL_UINT8(0x03, ap.boot);
    TEST_ASSERT_EQUAL(BL_ERR_BODY, bl_hb_ap_parse(b, 2, &ap));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_known_frame_bytes);
    RUN_TEST(test_empty_body);
    RUN_TEST(test_zero_bytes_in_body);
    RUN_TEST(test_max_frame);
    RUN_TEST(test_too_long_on_wire);
    RUN_TEST(test_small_output_buffer);
    RUN_TEST(test_crc_error_every_bit);
    RUN_TEST(test_short_and_bad_cobs);
    RUN_TEST(test_unknown_type);
    RUN_TEST(test_deframer_stream);
    RUN_TEST(test_deframer_overrun);
    RUN_TEST(test_relay_roundtrip);
    RUN_TEST(test_relay_rejects);
    RUN_TEST(test_subscribe);
    RUN_TEST(test_ack);
    RUN_TEST(test_heartbeats);
    return UNITY_END();
}
