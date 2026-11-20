/* The pure part of the NTRIP client (wiki Mower-Protocol §13): the request to
 * the caster, its answer, and whether a GGA sentence carries a position. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Longest status line plus headers accepted before the stream starts. */
#define NTRIP_HDR_MAX 1024

/* Writes the NTRIP rev 1 request (HTTP/1.0: the answer is never chunked) for
 * mountpoint on host:port with Basic auth, then gga (with its CR LF) when not
 * NULL. Returns the length, or -1 when out is too small or a setting would
 * break the request (empty host or mount point, space or control character in
 * host/mount point, control character or ':' in user, control character in
 * password). user NULL or "" sends no Authorization. */
int ntrip_build_request(char *out, size_t cap, const char *host, int port, const char *mountpoint,
                        const char *user, const char *password, const char *gga);

typedef enum {
    NTRIP_RESP_MORE,           /* no full answer yet: read more, call again with all bytes so far */
    NTRIP_RESP_STREAMING,      /* corrections follow at buf + *hdr_len */
    NTRIP_RESP_UNAUTHORIZED,   /* 401: wrong user or password */
    NTRIP_RESP_NO_MOUNT,       /* 404, or a source table: the mount point does not exist */
    NTRIP_RESP_ERROR,          /* anything else, chunked streams, or headers over NTRIP_HDR_MAX */
} ntrip_resp_t;

/* Parses the start of the caster's answer. buf holds every byte received so
 * far. On NTRIP_RESP_STREAMING *hdr_len is where the correction bytes start. */
ntrip_resp_t ntrip_parse_response(const char *buf, size_t len, size_t *hdr_len);

/* A $..GGA sentence (CR LF optional) with a correct checksum, a fix quality
 * other than 0 and a latitude and longitude: the client leaves "waiting" on the
 * first one (§13). */
bool nmea_gga_has_position(const char *s, size_t len);
