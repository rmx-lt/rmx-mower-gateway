/* The pure part of the NTRIP client (wiki Mower-Protocol §13): the request to
 * the caster, its answer, the chunked stream of rev 2, whether a GGA sentence
 * carries a position, and when to send it. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Longest status line plus headers accepted before the stream starts. */
#define NTRIP_HDR_MAX 1024
/* The caster wants a position at least this often (LitPOS: 10 s). */
#define NTRIP_GGA_PERIOD_MS 10000
/* Longest NMEA sentence kept (82 by the standard, plus CR LF and slack). */
#define NTRIP_GGA_MAX 100

/* Caster settings (ntrip_set). */
typedef struct {
    const char *host;
    int port;
    const char *mountpoint;
    const char *user;       /* NULL or "": no Authorization */
    const char *password;
} ntrip_cfg_t;

typedef enum {
    NTRIP_V1,   /* HTTP/1.0, answer "ICY 200 OK", raw stream; GGA follows the headers */
    NTRIP_V2,   /* HTTP/1.1 + Ntrip-Version: Ntrip/2.0; may be chunked; GGA in Ntrip-GGA */
} ntrip_ver_t;

/* Writes the request for cfg; gga (one sentence, CR LF optional) or NULL.
 * Returns the length, or -1 when out is too small or a setting would break the
 * request (empty host or mount point, bad port, space or control character in
 * host or mount point, control character or ':' in user, control character in
 * password or gga).
 * out then holds the credentials (Base64): send it, never log it; log
 * ntrip_request_masked() instead. The password appears nowhere else. */
int ntrip_build_request(char *out, size_t cap, const ntrip_cfg_t *cfg, ntrip_ver_t ver, const char *gga);

/* Copies request (len bytes) to out as a C string with the Authorization value
 * replaced by "***", for the log. Returns the length, or -1 when out is too
 * small. */
int ntrip_request_masked(const char *request, size_t len, char *out, size_t cap);

typedef enum {
    NTRIP_RESP_MORE,           /* no full answer yet: read more, call again with all bytes so far */
    NTRIP_RESP_STREAMING,      /* corrections follow at buf + *hdr_len (chunked when *chunked) */
    NTRIP_RESP_UNAUTHORIZED,   /* 401: wrong user or password */
    NTRIP_RESP_NO_MOUNT,       /* 404, or a source table: the mount point does not exist */
    NTRIP_RESP_ERROR,          /* anything else, or headers over NTRIP_HDR_MAX */
} ntrip_resp_t;

/* Parses the start of the caster's answer. buf holds every byte received so
 * far. On NTRIP_RESP_STREAMING *hdr_len is where the stream starts and
 * *chunked tells whether it must go through ntrip_chunk_decode(). */
ntrip_resp_t ntrip_parse_response(const char *buf, size_t len, size_t *hdr_len, bool *chunked);

/* Incremental decoder of HTTP chunked transfer coding. Zero-initialise. */
typedef struct {
    uint8_t state;
    uint8_t digits;
    uint32_t left;   /* bytes left in the current chunk */
    bool done;       /* the last (zero) chunk was seen */
} ntrip_chunk_t;

/* Decodes len bytes of the chunked stream into out (out may equal in: the
 * output never overtakes the input). Returns the number of data bytes written,
 * or -1 on malformed input (the connection should be dropped). After the last
 * chunk dec->done is set and the rest is ignored. */
int ntrip_chunk_decode(ntrip_chunk_t *dec, const uint8_t *in, size_t len, uint8_t *out);

/* A $..GGA sentence (CR LF optional) with a correct checksum, a fix quality
 * other than 0 and a latitude and longitude: the client leaves "waiting" on the
 * first one (§13). */
bool nmea_gga_has_position(const char *s, size_t len);

/* When to send the position to the caster. Zero-initialise. */
typedef struct {
    char gga[NTRIP_GGA_MAX + 1];   /* the newest sentence with a position */
    bool have;
    bool fresh;                    /* newer than the last one sent */
    bool sent_once;
    int64_t sent_ms;
} ntrip_gga_t;

/* A sentence from the autopilot; kept when it has a position (returns true). */
bool ntrip_gga_offer(ntrip_gga_t *g, const char *s, size_t len);

/* The sentence to send now (with CR LF), or NULL: a fresh one at once, else the
 * last one again NTRIP_GGA_PERIOD_MS after the previous send. Marks it sent. */
const char *ntrip_gga_due(ntrip_gga_t *g, int64_t now_ms);

/* A new connection: the next ntrip_gga_due() sends the kept sentence at once. */
void ntrip_gga_reconnected(ntrip_gga_t *g);
