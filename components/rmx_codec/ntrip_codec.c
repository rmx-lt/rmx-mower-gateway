#include "ntrip_codec.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ request */

static bool clean(const char *s, bool allow_space, bool allow_colon)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7F || (!allow_space && c == ' ') || (!allow_colon && c == ':')) {
            return false;
        }
    }
    return true;
}

typedef struct {
    char *out;
    size_t cap;
    size_t n;
    bool over;
} sink_t;

static void put(sink_t *k, const char *fmt, ...)
{
    if (k->over) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(k->out + k->n, k->cap - k->n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= k->cap - k->n) {
        k->over = true;
    } else {
        k->n += (size_t)r;
    }
}

static void put_base64(sink_t *k, const uint8_t *p, size_t len)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)p[i + 1] << 8;
        }
        if (i + 2 < len) {
            v |= p[i + 2];
        }
        char q[5] = { T[(v >> 18) & 63], T[(v >> 12) & 63], i + 1 < len ? T[(v >> 6) & 63] : '=',
                      i + 2 < len ? T[v & 63] : '=', '\0' };
        put(k, "%s", q);
    }
}

int ntrip_build_request(char *out, size_t cap, const char *host, int port, const char *mountpoint,
                        const char *user, const char *password, const char *gga)
{
    if (!out || cap == 0 || !host || !host[0] || !mountpoint || !mountpoint[0] || port < 1 || port > 65535 ||
            !clean(host, false, true) || !clean(mountpoint, false, true) ||
            (user && !clean(user, true, false)) || (password && !clean(password, true, true))) {
        return -1;
    }
    sink_t k = { out, cap, 0, false };
    put(&k, "GET /%s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: NTRIP rmx-gateway/1.0\r\n", mountpoint, host, port);
    if (user && user[0]) {
        char cred[256];
        int n = snprintf(cred, sizeof(cred), "%s:%s", user, password ? password : "");
        if (n < 0 || (size_t)n >= sizeof(cred)) {
            return -1;
        }
        put(&k, "Authorization: Basic ");
        put_base64(&k, (const uint8_t *)cred, (size_t)n);
        memset(cred, 0, sizeof(cred));
        put(&k, "\r\n");
    }
    put(&k, "\r\n");
    if (gga) {
        put(&k, "%s", gga);
    }
    return k.over ? -1 : (int)k.n;
}

/* ----------------------------------------------------------------- response */

/* Position just after the first "\r\n\r\n" (or "\n\n") in buf, or 0. */
static size_t header_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n' && buf[i + 1] == '\n') {
            return i + 2;
        }
        if (i + 3 < len && memcmp(buf + i, "\r\n\r\n", 4) == 0) {
            return i + 4;
        }
    }
    return 0;
}

static const char *find_line_end(const char *buf, size_t len)
{
    return memchr(buf, '\n', len);
}

static bool starts(const char *buf, size_t len, const char *p)
{
    size_t n = strlen(p);
    return len >= n && memcmp(buf, p, n) == 0;
}

/* Case-insensitive search of needle in buf[0..len). */
static bool contains_ci(const char *buf, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++) {
        size_t j = 0;
        while (j < n) {
            char a = buf[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
            j++;
        }
        if (j == n) {
            return true;
        }
    }
    return false;
}

ntrip_resp_t ntrip_parse_response(const char *buf, size_t len, size_t *hdr_len)
{
    const char *eol = find_line_end(buf, len);
    if (!eol) {
        return len >= NTRIP_HDR_MAX ? NTRIP_RESP_ERROR : NTRIP_RESP_MORE;
    }
    size_t line = (size_t)(eol - buf) + 1;

    if (starts(buf, len, "ICY ")) {
        if (!starts(buf, len, "ICY 200")) {
            return NTRIP_RESP_ERROR;
        }
        /* Rev 1: the stream follows the status line. Some casters add a blank
         * line; skip it when it is already here. shortcut: a blank line that
         * arrives in a later read reaches the receiver as 2 junk bytes, which
         * an RTCM parser skips (it syncs on 0xD3). */
        size_t h = line;
        if (len >= h + 2 && buf[h] == '\r' && buf[h + 1] == '\n') {
            h += 2;
        }
        *hdr_len = h;
        return NTRIP_RESP_STREAMING;
    }
    if (starts(buf, len, "SOURCETABLE ")) {
        return NTRIP_RESP_NO_MOUNT;   /* the caster lists its mount points instead */
    }
    if (!starts(buf, len, "HTTP/1.")) {
        return NTRIP_RESP_ERROR;
    }
    int code = 0;
    if (line < 13 || sscanf(buf + 9, "%3d", &code) != 1) {
        return NTRIP_RESP_ERROR;
    }
    if (code == 401) {
        return NTRIP_RESP_UNAUTHORIZED;
    }
    if (code == 404) {
        return NTRIP_RESP_NO_MOUNT;
    }
    if (code != 200) {
        return NTRIP_RESP_ERROR;
    }
    size_t end = header_end(buf, len < NTRIP_HDR_MAX ? len : NTRIP_HDR_MAX);
    if (!end) {
        return len >= NTRIP_HDR_MAX ? NTRIP_RESP_ERROR : NTRIP_RESP_MORE;
    }
    if (contains_ci(buf, end, "content-type: gnss/sourcetable")) {
        return NTRIP_RESP_NO_MOUNT;
    }
    if (contains_ci(buf, end, "transfer-encoding: chunked")) {
        return NTRIP_RESP_ERROR;   /* rev 2 chunking: not supported (we ask for rev 1) */
    }
    *hdr_len = end;
    return NTRIP_RESP_STREAMING;
}

/* --------------------------------------------------------------------- NMEA */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool nmea_gga_has_position(const char *s, size_t len)
{
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n')) {
        len--;
    }
    /* $ttGGA,...*HH */
    if (len < 10 || s[0] != '$' || memcmp(s + 3, "GGA,", 4) != 0 || s[len - 3] != '*') {
        return false;
    }
    uint8_t sum = 0;
    for (size_t i = 1; i < len - 3; i++) {
        sum ^= (uint8_t)s[i];
    }
    int hi = hexval(s[len - 2]), lo = hexval(s[len - 1]);
    if (hi < 0 || lo < 0 || sum != (uint8_t)(hi << 4 | lo)) {
        return false;
    }
    /* Fields after "$ttGGA,": 0 time, 1 lat, 2 N/S, 3 lon, 4 E/W, 5 quality. */
    size_t start[7] = { 0 }, flen[7] = { 0 };
    size_t f = 0, p = 7, body_end = len - 3;
    start[0] = p;
    for (; p < body_end && f < 6; p++) {
        if (s[p] == ',') {
            flen[f] = p - start[f];
            start[++f] = p + 1;
        }
    }
    if (f < 6) {
        return false;   /* fewer than 7 fields */
    }
    bool ns = flen[2] == 1 && (s[start[2]] == 'N' || s[start[2]] == 'S');
    bool ew = flen[4] == 1 && (s[start[4]] == 'E' || s[start[4]] == 'W');
    size_t q = start[5];
    bool fix = flen[5] == 1 && s[q] >= '1' && s[q] <= '9';
    return flen[1] > 0 && ns && flen[3] > 0 && ew && fix;
}
