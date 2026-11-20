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

/* Base64 of the concatenated strings parts[0..n), without joining them in a
 * buffer: the password is never copied anywhere but the request. */
static void put_base64(sink_t *k, const char *const *parts, int n)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int part = 0;
    const char *p = parts[0];
    for (;;) {
        uint8_t b[3];
        int got = 0;
        while (got < 3 && part < n) {
            if (*p) {
                b[got++] = (uint8_t)*p++;
            } else if (++part < n) {
                p = parts[part];
            }
        }
        if (got == 0) {
            return;
        }
        uint32_t v = (uint32_t)b[0] << 16 | (got > 1 ? (uint32_t)b[1] << 8 : 0) | (got > 2 ? b[2] : 0);
        char q[5] = { T[(v >> 18) & 63], T[(v >> 12) & 63], got > 1 ? T[(v >> 6) & 63] : '=',
                      got > 2 ? T[v & 63] : '=', '\0' };
        put(k, "%s", q);
        if (got < 3) {
            return;
        }
    }
}

/* Length of s without a trailing CR LF. */
static size_t line_len(const char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) {
        n--;
    }
    return n;
}

int ntrip_build_request(char *out, size_t cap, const ntrip_cfg_t *cfg, ntrip_ver_t ver, const char *gga)
{
    if (!out || cap == 0 || !cfg || !cfg->host || !cfg->host[0] || !cfg->mountpoint || !cfg->mountpoint[0] ||
            cfg->port < 1 || cfg->port > 65535 || !clean(cfg->host, false, true) ||
            !clean(cfg->mountpoint, false, true) || (cfg->user && !clean(cfg->user, true, false)) ||
            (cfg->password && !clean(cfg->password, true, true))) {
        return -1;
    }
    size_t gl = gga ? line_len(gga) : 0;
    for (size_t i = 0; i < gl; i++) {
        if ((unsigned char)gga[i] < 0x20) {
            return -1;
        }
    }
    sink_t k = { out, cap, 0, false };
    put(&k, "GET /%s HTTP/1.%c\r\nHost: %s:%d\r\n", cfg->mountpoint, ver == NTRIP_V2 ? '1' : '0', cfg->host,
        cfg->port);
    if (ver == NTRIP_V2) {
        put(&k, "Ntrip-Version: Ntrip/2.0\r\n");
    }
    put(&k, "User-Agent: NTRIP rmx-gateway/1.0\r\n");
    if (cfg->user && cfg->user[0]) {
        const char *parts[3] = { cfg->user, ":", cfg->password ? cfg->password : "" };
        put(&k, "Authorization: Basic ");
        put_base64(&k, parts, 3);
        put(&k, "\r\n");
    }
    if (ver == NTRIP_V2) {
        if (gl) {
            put(&k, "Ntrip-GGA: %.*s\r\n", (int)gl, gga);
        }
        put(&k, "Connection: close\r\n\r\n");
    } else {
        put(&k, "\r\n");
        if (gl) {
            put(&k, "%.*s\r\n", (int)gl, gga);
        }
    }
    return k.over ? -1 : (int)k.n;
}

int ntrip_request_masked(const char *request, size_t len, char *out, size_t cap)
{
    static const char AUTH[] = "Authorization: ";
    static const char MASK[] = "Authorization: ***";
    size_t n = 0;
    size_t i = 0;
    while (i < len) {
        bool line_start = i == 0 || request[i - 1] == '\n';
        if (line_start && len - i >= sizeof(AUTH) - 1 && memcmp(request + i, AUTH, sizeof(AUTH) - 1) == 0) {
            if (n + sizeof(MASK) - 1 >= cap) {
                return -1;
            }
            memcpy(out + n, MASK, sizeof(MASK) - 1);
            n += sizeof(MASK) - 1;
            while (i < len && request[i] != '\r' && request[i] != '\n') {
                i++;   /* skip the credentials */
            }
            continue;
        }
        if (n + 1 >= cap) {
            return -1;
        }
        out[n++] = request[i++];
    }
    if (cap == 0) {
        return -1;
    }
    out[n] = '\0';
    return (int)n;
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

ntrip_resp_t ntrip_parse_response(const char *buf, size_t len, size_t *hdr_len, bool *chunked)
{
    *chunked = false;
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
    *chunked = contains_ci(buf, end, "transfer-encoding: chunked");
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

/* ------------------------------------------------------------------ chunked */

enum { CH_SIZE, CH_EXT, CH_SIZE_LF, CH_DATA, CH_DATA_CR, CH_DATA_LF, CH_DONE };

int ntrip_chunk_decode(ntrip_chunk_t *d, const uint8_t *in, size_t len, uint8_t *out)
{
    size_t w = 0;
    for (size_t i = 0; i < len && d->state != CH_DONE; i++) {
        uint8_t c = in[i];
        switch (d->state) {
        case CH_SIZE: {
            int h = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (h >= 0) {
                if (++d->digits > 7) {
                    return -1;   /* 256 MB or more: nonsense */
                }
                d->left = d->left << 4 | (uint32_t)h;
            } else if (d->digits == 0) {
                return -1;
            } else if (c == ';' || c == ' ' || c == '\t') {
                d->state = CH_EXT;
            } else if (c == '\r') {
                d->state = CH_SIZE_LF;
            } else {
                return -1;
            }
            break;
        }
        case CH_EXT:   /* chunk extensions are ignored */
            if (c == '\r') {
                d->state = CH_SIZE_LF;
            }
            break;
        case CH_SIZE_LF:
            if (c != '\n') {
                return -1;
            }
            d->digits = 0;
            if (d->left == 0) {
                d->state = CH_DONE;   /* trailers and the final CR LF are ignored */
                d->done = true;
            } else {
                d->state = CH_DATA;
            }
            break;
        case CH_DATA: {
            size_t n = len - i < d->left ? len - i : d->left;
            memmove(out + w, in + i, n);
            w += n;
            d->left -= (uint32_t)n;
            i += n - 1;
            if (d->left == 0) {
                d->state = CH_DATA_CR;
            }
            break;
        }
        case CH_DATA_CR:
            if (c != '\r') {
                return -1;
            }
            d->state = CH_DATA_LF;
            break;
        default:   /* CH_DATA_LF */
            if (c != '\n') {
                return -1;
            }
            d->state = CH_SIZE;
            break;
        }
    }
    return (int)w;
}

/* --------------------------------------------------------------- GGA timing */

bool ntrip_gga_offer(ntrip_gga_t *g, const char *s, size_t len)
{
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n')) {
        len--;
    }
    if (len + 2 > NTRIP_GGA_MAX || !nmea_gga_has_position(s, len)) {
        return false;
    }
    memcpy(g->gga, s, len);
    memcpy(g->gga + len, "\r\n", 3);
    g->have = true;
    g->fresh = true;
    return true;
}

const char *ntrip_gga_due(ntrip_gga_t *g, int64_t now_ms)
{
    if (!g->have || (!g->fresh && g->sent_once && now_ms - g->sent_ms < NTRIP_GGA_PERIOD_MS)) {
        return NULL;
    }
    g->fresh = false;
    g->sent_once = true;
    g->sent_ms = now_ms;
    return g->gga;
}

void ntrip_gga_reconnected(ntrip_gga_t *g)
{
    g->fresh = g->have;
}
