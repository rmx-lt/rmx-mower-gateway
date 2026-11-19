/* Board link frames and bodies (wiki Mower-Protocol §14). Pure C, no RTOS.
 *
 * On the wire: COBS( type | body | CRC-16 little-endian ) 0x00, where the CRC
 * (CRC-16/CCITT-FALSE) covers type and body. type + body + CRC is at most
 * BL_FRAME_MAX bytes. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cobs.h"

#define BL_FRAME_MAX     4608                          /* type + body + CRC, before COBS */
#define BL_BODY_MAX      (BL_FRAME_MAX - 3)
#define BL_WIRE_MAX      (COBS_MAX_ENCODED(BL_FRAME_MAX) + 1)   /* encoded + 0x00 */
#define BL_TOPIC_MAX     64
#define BL_LINK_VERSION  1

typedef enum {
    BL_PUBLISH     = 0x01,   /* autopilot -> gateway */
    BL_MESSAGE     = 0x02,   /* gateway -> autopilot */
    BL_SUBSCRIBE   = 0x03,   /* autopilot -> gateway */
    BL_CORRECTIONS = 0x04,   /* gateway -> autopilot, raw caster bytes */
    BL_GGA         = 0x05,   /* autopilot -> gateway, one NMEA sentence */
    BL_HEARTBEAT   = 0x06,   /* both */
    BL_ACK         = 0x07,   /* both */
} bl_type_t;

typedef enum {
    BL_OK = 0,
    BL_ERR_COBS,       /* not valid COBS */
    BL_ERR_SHORT,      /* fewer than 3 bytes (type + CRC) */
    BL_ERR_TOO_LONG,   /* over BL_FRAME_MAX, or the output buffer is too small */
    BL_ERR_CRC,
    BL_ERR_TYPE,       /* unknown type */
    BL_ERR_BODY,       /* a body that does not follow §14 */
} bl_err_t;

typedef struct {
    uint8_t type;
    const uint8_t *body;   /* points into the decode buffer */
    size_t body_len;
} bl_frame_t;

/* ------------------------------------------------------------------ frames */

/* Writes the whole wire frame (COBS bytes and the trailing 0x00) to out.
 * Returns its length, or 0 when body_len > BL_BODY_MAX or out is too small
 * (BL_WIRE_MAX always fits). The type is not checked, so tests can send
 * unknown ones. */
size_t bl_frame_encode(uint8_t type, const uint8_t *body, size_t body_len, uint8_t *out, size_t cap);

/* Decodes one frame's COBS bytes (without the 0x00) into buf (BL_FRAME_MAX
 * bytes are enough) and checks length, CRC and type. f->body points into buf. */
bl_err_t bl_frame_decode(const uint8_t *wire, size_t len, uint8_t *buf, size_t cap, bl_frame_t *f);

/* Splits a byte stream at 0x00. buf holds one encoded frame (BL_WIRE_MAX).
 * A frame longer than buf is discarded up to the next 0x00 and counted. */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool discarding;
    uint32_t overruns;
} bl_deframer_t;

void bl_deframer_init(bl_deframer_t *d, uint8_t *buf, size_t cap);
/* Feeds one byte. True at a 0x00 that ends a non-empty frame: *frame and *len
 * are its COBS bytes, valid until the next call. Empty frames are skipped. */
bool bl_deframer_push(bl_deframer_t *d, uint8_t byte, const uint8_t **frame, size_t *len);

/* ------------------------------------------------------------------ bodies */

/* PUBLISH and MESSAGE (§14.2). seq is 0 for QoS 0 and 1..255 for QoS 1. */
typedef struct {
    bool retain;
    uint8_t qos;
    uint8_t seq;
    const char *topic;     /* without the mower/<name>/autopilot/ prefix, not NUL-terminated */
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
} bl_relay_t;

/* Body length, or 0 when a field is out of range or out is too small. */
size_t bl_relay_encode(const bl_relay_t *r, uint8_t *out, size_t cap);
/* BL_ERR_BODY when the topic is empty, over 64 bytes or not printable ASCII
 * without wildcards, QoS is not 0/1, or seq does not match QoS. Unknown flag
 * bits are ignored. Pointers point into body. */
bl_err_t bl_relay_parse(const uint8_t *body, size_t len, bl_relay_t *r);

/* SUBSCRIBE (§14.2): seq, then entries of QoS, topic length, topic. */
typedef struct {
    uint8_t qos;
    const char *topic;
    size_t topic_len;
} bl_sub_entry_t;

size_t bl_subscribe_encode(uint8_t seq, const bl_sub_entry_t *e, size_t n, uint8_t *out, size_t cap);
/* At least one entry, seq 1..255, the same topic rules as PUBLISH; more than
 * max entries is BL_ERR_BODY. */
bl_err_t bl_subscribe_parse(const uint8_t *body, size_t len, uint8_t *seq, bl_sub_entry_t *e, size_t max,
                            size_t *n);

/* ACK (§14.2): one byte, the sequence number 1..255. */
bl_err_t bl_ack_parse(const uint8_t *body, size_t len, uint8_t *seq);

/* HEARTBEAT (§14.4). */
#define BL_HB_MQTT          0x01
#define BL_HB_SERVER        0x02
#define BL_HB_CORRECTIONS   0x04
#define BL_HB_CLOCK         0x08
#define BL_HB_SUBS_NEEDED   0x10

typedef struct {
    uint8_t version;
    uint8_t flags;
    uint32_t unix_s;     /* 0 when the clock is not valid */
    uint16_t ms;
} bl_hb_gw_t;

#define BL_HB_GW_LEN 8
#define BL_HB_AP_LEN 2

size_t bl_hb_gw_encode(const bl_hb_gw_t *h, uint8_t *out, size_t cap);
/* At least 8 bytes; extra bytes are ignored. The version is returned, not
 * checked: the caller keeps the link down on an unknown one. */
bl_err_t bl_hb_gw_parse(const uint8_t *body, size_t len, bl_hb_gw_t *h);
size_t bl_hb_ap_encode(uint8_t version, uint8_t *out, size_t cap);
bl_err_t bl_hb_ap_parse(const uint8_t *body, size_t len, uint8_t *version);
