/* Host tests of components/rmx_codec/ntrip_codec.c (wiki Mower-Protocol §13).
 * Run: pio test -e native */
#include <string.h>
#include <unity.h>
#include "ntrip_codec.h"

void setUp(void) {}
void tearDown(void) {}

static char out[512];

/* The rev 1 request from loose settings. */
static int req1(char *o, size_t cap, const char *host, int port, const char *mnt, const char *user, const char *pw,
                const char *gga)
{
    ntrip_cfg_t c = { host, port, mnt, user, pw };
    return ntrip_build_request(o, cap, &c, NTRIP_V1, gga);
}

static void test_request_text(void)
{
    int n = req1(out, sizeof(out), "caster.example", 2101, "MNT1", "user", "pass", NULL);
    const char *want = "GET /MNT1 HTTP/1.0\r\n"
                       "Host: caster.example:2101\r\n"
                       "User-Agent: NTRIP rmx-gateway/1.0\r\n"
                       "Authorization: Basic dXNlcjpwYXNz\r\n"
                       "\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(want), n);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

static void test_request_base64_padding(void)
{
    TEST_ASSERT_GREATER_THAN(0, req1(out, sizeof(out), "h", 1, "m", "ab", "c", NULL));
    TEST_ASSERT_NOT_NULL(strstr(out, "Basic YWI6Yw==\r\n"));
    TEST_ASSERT_GREATER_THAN(0, req1(out, sizeof(out), "h", 1, "m", "u", "p1", NULL));
    TEST_ASSERT_NOT_NULL(strstr(out, "Basic dTpwMQ==\r\n"));
}

static void test_request_no_user_and_gga(void)
{
    const char *gga = "$GPGGA,1*00\r\n";   /* the sentence itself is the caller's business */
    int n = req1(out, sizeof(out), "h", 2101, "m", "", NULL, gga);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NULL(strstr(out, "Authorization"));
    TEST_ASSERT_EQUAL_STRING(gga, out + n - strlen(gga));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\n\r\n$GPGGA"));
}

static void test_request_refuses_bad_settings(void)
{
    TEST_ASSERT_EQUAL_INT(-1, req1(out, sizeof(out), "", 2101, "m", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, req1(out, sizeof(out), "h", 2101, "", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, req1(out, sizeof(out), "h", 0, "m", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, req1(out, sizeof(out), "h", 70000, "m", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, req1(out, sizeof(out), "h", 1, "m x", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, req1(out, sizeof(out), "h\r\nX: y", 1, "m", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, req1(out, sizeof(out), "h", 1, "m", "a:b", "p", NULL));
    TEST_ASSERT_EQUAL_INT(-1, req1(out, sizeof(out), "h", 1, "m", "u", "p\n", NULL));
    TEST_ASSERT_GREATER_THAN(0, req1(out, sizeof(out), "h", 1, "m", "u", "p a:b", NULL));
}

static void test_request_too_small(void)
{
    char small[40];
    TEST_ASSERT_EQUAL_INT(-1, req1(small, sizeof(small), "caster.example", 2101, "MNT1", "user",
                                                  "pass", NULL));
}

static bool chunked;

static ntrip_resp_t parse(const char *s, size_t *h)
{
    return ntrip_parse_response(s, strlen(s), h, &chunked);
}

static void test_icy_ok(void)
{
    size_t h = 0;
    const char r[] = "ICY 200 OK\r\n\xD3\x00\x13";
    TEST_ASSERT_EQUAL(NTRIP_RESP_STREAMING, ntrip_parse_response(r, sizeof(r) - 1, &h, &chunked));
    TEST_ASSERT_EQUAL_UINT(12, h);
    TEST_ASSERT_EQUAL(NTRIP_RESP_STREAMING, parse("ICY 200 OK\r\n\r\n", &h));
    TEST_ASSERT_EQUAL_UINT(14, h);
}

static void test_http_ok(void)
{
    size_t h = 0;
    const char *r = "HTTP/1.1 200 OK\r\nContent-Type: gnss/data\r\n\r\nXY";
    TEST_ASSERT_EQUAL(NTRIP_RESP_STREAMING, parse(r, &h));
    TEST_ASSERT_EQUAL_STRING("XY", r + h);
    TEST_ASSERT_FALSE(chunked);
    TEST_ASSERT_EQUAL(NTRIP_RESP_STREAMING, parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n", &h));
    TEST_ASSERT_TRUE(chunked);
}

static void test_split_reads(void)
{
    size_t h = 0;
    TEST_ASSERT_EQUAL(NTRIP_RESP_MORE, parse("", &h));
    TEST_ASSERT_EQUAL(NTRIP_RESP_MORE, parse("ICY 20", &h));
    TEST_ASSERT_EQUAL(NTRIP_RESP_MORE, parse("HTTP/1.1 200 OK\r\nServer: x\r\n", &h));
    TEST_ASSERT_EQUAL(NTRIP_RESP_STREAMING, parse("HTTP/1.1 200 OK\r\nServer: x\r\n\r\n", &h));
}

static void test_errors(void)
{
    size_t h = 0;
    TEST_ASSERT_EQUAL(NTRIP_RESP_UNAUTHORIZED, parse("HTTP/1.0 401 Unauthorized\r\n", &h));
    TEST_ASSERT_EQUAL(NTRIP_RESP_NO_MOUNT, parse("HTTP/1.1 404 Not Found\r\n", &h));
    TEST_ASSERT_EQUAL(NTRIP_RESP_NO_MOUNT, parse("SOURCETABLE 200 OK\r\n", &h));
    TEST_ASSERT_EQUAL(NTRIP_RESP_NO_MOUNT,
                      parse("HTTP/1.1 200 OK\r\nContent-Type: gnss/sourcetable\r\n\r\nSTR;", &h));
    TEST_ASSERT_EQUAL(NTRIP_RESP_ERROR, parse("HTTP/1.1 500 Oops\r\n", &h));
    TEST_ASSERT_EQUAL(NTRIP_RESP_ERROR, parse("ICY 401 Unauthorized\r\n", &h));
    TEST_ASSERT_EQUAL(NTRIP_RESP_ERROR, parse("garbage\r\n", &h));
}

static void test_header_too_long(void)
{
    static char big[NTRIP_HDR_MAX + 64];
    strcpy(big, "HTTP/1.1 200 OK\r\n");
    size_t n = strlen(big);
    while (n < NTRIP_HDR_MAX + 10) {
        big[n++] = 'a';
    }
    size_t h = 0;
    TEST_ASSERT_EQUAL(NTRIP_RESP_ERROR, ntrip_parse_response(big, n, &h, &chunked));
    memset(big, 'a', NTRIP_HDR_MAX);   /* no line end at all */
    TEST_ASSERT_EQUAL(NTRIP_RESP_ERROR, ntrip_parse_response(big, NTRIP_HDR_MAX, &h, &chunked));
}

static bool gga(const char *s)
{
    return nmea_gga_has_position(s, strlen(s));
}

static void test_gga(void)
{
    TEST_ASSERT_TRUE(gga("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    TEST_ASSERT_TRUE(gga("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"));
    TEST_ASSERT_TRUE(gga("$GNGGA,101010.00,5441.1234,N,02516.5678,E,4,12,0.6,120.3,M,28.1,M,1.0,0000*5B"));
    TEST_ASSERT_TRUE(gga("$GNGGA,101010.00,5441.1234,N,02516.5678,E,4,12,0.6,120.3,M,28.1,M,1.0,0000*5b"));
    /* no fix, empty position */
    TEST_ASSERT_FALSE(gga("$GNGGA,101010.00,,,,,0,00,99.99,,,,,,*79"));
    /* bad checksum, no checksum, other sentence, truncated */
    TEST_ASSERT_FALSE(gga("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48"));
    TEST_ASSERT_FALSE(gga("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"));
    TEST_ASSERT_FALSE(gga("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A"));
    TEST_ASSERT_FALSE(gga("$GPGGA,1*00"));
    TEST_ASSERT_FALSE(gga(""));
}


static const char GGA[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";

static void test_request_v2(void)
{
    ntrip_cfg_t c = { "caster.example", 2101, "MNT1", "user", "pass" };
    int n = ntrip_build_request(out, sizeof(out), &c, NTRIP_V2, GGA);
    const char *want = "GET /MNT1 HTTP/1.1\r\n"
                       "Host: caster.example:2101\r\n"
                       "Ntrip-Version: Ntrip/2.0\r\n"
                       "User-Agent: NTRIP rmx-gateway/1.0\r\n"
                       "Authorization: Basic dXNlcjpwYXNz\r\n"
                       "Ntrip-GGA: $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"
                       "Connection: close\r\n"
                       "\r\n";
    TEST_ASSERT_EQUAL_STRING(want, out);
    TEST_ASSERT_EQUAL_INT((int)strlen(want), n);
}

static void test_request_v1_gga_gets_crlf(void)
{
    int n = req1(out, sizeof(out), "h", 1, "m", NULL, NULL, GGA);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING("\r\n\r\n$GPGGA", strstr(out, "\r\n\r\n$GPGGA") ? "\r\n\r\n$GPGGA" : "missing");
    TEST_ASSERT_EQUAL_STRING("*47\r\n", out + n - 5);
}

static void test_request_refuses_gga_injection(void)
{
    ntrip_cfg_t c = { "h", 1, "m", NULL, NULL };
    TEST_ASSERT_EQUAL_INT(-1, ntrip_build_request(out, sizeof(out), &c, NTRIP_V2, "$GPGGA\r\nX: y"));
}

static void test_masked_request_has_no_credentials(void)
{
    ntrip_cfg_t c = { "h", 2101, "m", "user", "s3cret-pw" };
    int n = ntrip_build_request(out, sizeof(out), &c, NTRIP_V2, NULL);
    char log[512];
    int m = ntrip_request_masked(out, (size_t)n, log, sizeof(log));
    TEST_ASSERT_GREATER_THAN(0, m);
    TEST_ASSERT_NOT_NULL(strstr(log, "Authorization: ***\r\n"));
    TEST_ASSERT_NULL(strstr(log, "dXNlcjpzM2NyZXQtcHc="));
    TEST_ASSERT_NULL(strstr(log, "s3cret"));
    TEST_ASSERT_NOT_NULL(strstr(log, "Host: h:2101\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "Basic dXNlcjpzM2NyZXQtcHc=\r\n"));   /* the request itself keeps them */
    char tiny[8];
    TEST_ASSERT_EQUAL_INT(-1, ntrip_request_masked(out, (size_t)n, tiny, sizeof(tiny)));
}

/* Feeds src to the decoder in pieces of step bytes (in place). */
static int decode_steps(ntrip_chunk_t *d, const char *src, size_t len, size_t step, char *dst)
{
    char buf[256];
    int total = 0;
    for (size_t i = 0; i < len; i += step) {
        size_t n = len - i < step ? len - i : step;
        memcpy(buf, src + i, n);
        int r = ntrip_chunk_decode(d, (uint8_t *)buf, n, (uint8_t *)buf);
        if (r < 0) {
            return -1;
        }
        memcpy(dst + total, buf, (size_t)r);
        total += r;
    }
    dst[total] = '\0';
    return total;
}

static void test_chunked_any_split(void)
{
    const char src[] = "4\r\nWiki\r\n6;ext=1\r\npedia \r\nE\r\nin \r\n\r\nchunks.\r\n0\r\nTrailer: x\r\n\r\nJUNK";
    for (size_t step = 1; step <= sizeof(src); step++) {
        ntrip_chunk_t d;
        memset(&d, 0, sizeof(d));
        char dst[128];
        int n = decode_steps(&d, src, sizeof(src) - 1, step, dst);
        TEST_ASSERT_EQUAL_INT_MESSAGE(24, n, "length");
        TEST_ASSERT_EQUAL_STRING("Wikipedia in \r\n\r\nchunks.", dst);
        TEST_ASSERT_TRUE(d.done);
    }
}

static void test_chunked_binary_and_upper_hex(void)
{
    const uint8_t src[] = { '1', 'A', '\r', '\n' };
    ntrip_chunk_t d;
    memset(&d, 0, sizeof(d));
    uint8_t out_b[64];
    TEST_ASSERT_EQUAL_INT(0, ntrip_chunk_decode(&d, src, sizeof(src), out_b));
    uint8_t data[26];
    for (int i = 0; i < 26; i++) {
        data[i] = (uint8_t)(0xD3 + i);
    }
    TEST_ASSERT_EQUAL_INT(26, ntrip_chunk_decode(&d, data, sizeof(data), out_b));
    TEST_ASSERT_EQUAL_MEMORY(data, out_b, 26);
    TEST_ASSERT_FALSE(d.done);
}

static void test_chunked_malformed(void)
{
    const char *bad[] = { "x\r\n", "\r\n", "4\rX", "4\r\nabcdX", "4\r\nabcd\rX", "12345678\r\n" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ntrip_chunk_t d;
        memset(&d, 0, sizeof(d));
        uint8_t o[64];
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, ntrip_chunk_decode(&d, (const uint8_t *)bad[i], strlen(bad[i]), o), bad[i]);
    }
}

static void test_gga_schedule(void)
{
    ntrip_gga_t g;
    memset(&g, 0, sizeof(g));
    TEST_ASSERT_NULL(ntrip_gga_due(&g, 0));   /* waiting: nothing to send */
    TEST_ASSERT_FALSE(ntrip_gga_offer(&g, "$GNGGA,101010.00,,,,,0,00,99.99,,,,,,*79", 40));
    TEST_ASSERT_NULL(ntrip_gga_due(&g, 100));
    TEST_ASSERT_TRUE(ntrip_gga_offer(&g, GGA, strlen(GGA)));
    const char *s = ntrip_gga_due(&g, 200);   /* fresh: at once */
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("*47\r\n", s + strlen(s) - 5);
    TEST_ASSERT_NULL(ntrip_gga_due(&g, 300));
    TEST_ASSERT_NULL(ntrip_gga_due(&g, 200 + NTRIP_GGA_PERIOD_MS - 1));
    TEST_ASSERT_NOT_NULL(ntrip_gga_due(&g, 200 + NTRIP_GGA_PERIOD_MS));   /* no newer one: the same again */
    TEST_ASSERT_TRUE(ntrip_gga_offer(&g, GGA, strlen(GGA)));
    TEST_ASSERT_NOT_NULL(ntrip_gga_due(&g, 10300));   /* a new one goes out at once */
    TEST_ASSERT_NULL(ntrip_gga_due(&g, 10400));
    ntrip_gga_reconnected(&g);
    TEST_ASSERT_NOT_NULL(ntrip_gga_due(&g, 10500));   /* new connection: at once */
    /* a sentence without a position keeps the last good one */
    TEST_ASSERT_FALSE(ntrip_gga_offer(&g, "$GNGGA,101010.00,,,,,0,00,99.99,,,,,,*79", 40));
    TEST_ASSERT_NULL(ntrip_gga_due(&g, 10600));
    TEST_ASSERT_NOT_NULL(ntrip_gga_due(&g, 10500 + NTRIP_GGA_PERIOD_MS));
}

static void test_gga_schedule_too_long(void)
{
    ntrip_gga_t g;
    memset(&g, 0, sizeof(g));
    char longs[NTRIP_GGA_MAX + 10];
    memset(longs, 'A', sizeof(longs));
    TEST_ASSERT_FALSE(ntrip_gga_offer(&g, longs, sizeof(longs)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_request_text);
    RUN_TEST(test_request_base64_padding);
    RUN_TEST(test_request_no_user_and_gga);
    RUN_TEST(test_request_refuses_bad_settings);
    RUN_TEST(test_request_too_small);
    RUN_TEST(test_icy_ok);
    RUN_TEST(test_http_ok);
    RUN_TEST(test_split_reads);
    RUN_TEST(test_errors);
    RUN_TEST(test_header_too_long);
    RUN_TEST(test_gga);
    RUN_TEST(test_request_v2);
    RUN_TEST(test_request_v1_gga_gets_crlf);
    RUN_TEST(test_request_refuses_gga_injection);
    RUN_TEST(test_masked_request_has_no_credentials);
    RUN_TEST(test_chunked_any_split);
    RUN_TEST(test_chunked_binary_and_upper_hex);
    RUN_TEST(test_chunked_malformed);
    RUN_TEST(test_gga_schedule);
    RUN_TEST(test_gga_schedule_too_long);
    return UNITY_END();
}
