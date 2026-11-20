/* Host tests of components/rmx_codec/ntrip_codec.c (wiki Mower-Protocol §13).
 * Run: pio test -e native */
#include <string.h>
#include <unity.h>
#include "ntrip_codec.h"

void setUp(void) {}
void tearDown(void) {}

static char out[512];

static void test_request_text(void)
{
    int n = ntrip_build_request(out, sizeof(out), "caster.example", 2101, "MNT1", "user", "pass", NULL);
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
    TEST_ASSERT_GREATER_THAN(0, ntrip_build_request(out, sizeof(out), "h", 1, "m", "ab", "c", NULL));
    TEST_ASSERT_NOT_NULL(strstr(out, "Basic YWI6Yw==\r\n"));
    TEST_ASSERT_GREATER_THAN(0, ntrip_build_request(out, sizeof(out), "h", 1, "m", "u", "p1", NULL));
    TEST_ASSERT_NOT_NULL(strstr(out, "Basic dTpwMQ==\r\n"));
}

static void test_request_no_user_and_gga(void)
{
    const char *gga = "$GPGGA,1*00\r\n";
    int n = ntrip_build_request(out, sizeof(out), "h", 2101, "m", "", NULL, gga);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NULL(strstr(out, "Authorization"));
    TEST_ASSERT_EQUAL_STRING(gga, out + n - strlen(gga));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\n\r\n$GPGGA"));
}

static void test_request_refuses_bad_settings(void)
{
    TEST_ASSERT_EQUAL_INT(-1, ntrip_build_request(out, sizeof(out), "", 2101, "m", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, ntrip_build_request(out, sizeof(out), "h", 2101, "", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, ntrip_build_request(out, sizeof(out), "h", 0, "m", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, ntrip_build_request(out, sizeof(out), "h", 70000, "m", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, ntrip_build_request(out, sizeof(out), "h", 1, "m x", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, ntrip_build_request(out, sizeof(out), "h\r\nX: y", 1, "m", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, ntrip_build_request(out, sizeof(out), "h", 1, "m", "a:b", "p", NULL));
    TEST_ASSERT_EQUAL_INT(-1, ntrip_build_request(out, sizeof(out), "h", 1, "m", "u", "p\n", NULL));
    TEST_ASSERT_GREATER_THAN(0, ntrip_build_request(out, sizeof(out), "h", 1, "m", "u", "p a:b", NULL));
}

static void test_request_too_small(void)
{
    char small[40];
    TEST_ASSERT_EQUAL_INT(-1, ntrip_build_request(small, sizeof(small), "caster.example", 2101, "MNT1", "user",
                                                  "pass", NULL));
}

static ntrip_resp_t parse(const char *s, size_t *h)
{
    return ntrip_parse_response(s, strlen(s), h);
}

static void test_icy_ok(void)
{
    size_t h = 0;
    const char r[] = "ICY 200 OK\r\n\xD3\x00\x13";
    TEST_ASSERT_EQUAL(NTRIP_RESP_STREAMING, ntrip_parse_response(r, sizeof(r) - 1, &h));
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
    TEST_ASSERT_EQUAL(NTRIP_RESP_ERROR, parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", &h));
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
    TEST_ASSERT_EQUAL(NTRIP_RESP_ERROR, ntrip_parse_response(big, n, &h));
    memset(big, 'a', NTRIP_HDR_MAX);   /* no line end at all */
    TEST_ASSERT_EQUAL(NTRIP_RESP_ERROR, ntrip_parse_response(big, NTRIP_HDR_MAX, &h));
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
    return UNITY_END();
}
