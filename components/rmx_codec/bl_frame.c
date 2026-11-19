#include "bl_frame.h"

#include <string.h>
#include "crc16.h"

/* ------------------------------------------------------------------ frames */

size_t bl_frame_encode(uint8_t type, const uint8_t *body, size_t body_len, uint8_t *out, size_t cap)
{
    if (body_len > BL_BODY_MAX || cap < 1) {
        return 0;
    }
    uint16_t crc = crc16_update(CRC16_INIT, &type, 1);
    crc = crc16_update(crc, body, body_len);
    cobs_enc_t e;
    cobs_enc_begin(&e, out, cap - 1);   /* room for the 0x00 */
    cobs_enc_put(&e, type);
    for (size_t i = 0; i < body_len; i++) {
        cobs_enc_put(&e, body[i]);
    }
    cobs_enc_put(&e, (uint8_t)(crc & 0xFF));
    cobs_enc_put(&e, (uint8_t)(crc >> 8));
    size_t n = cobs_enc_end(&e);
    if (n == 0) {
        return 0;
    }
    out[n] = 0x00;
    return n + 1;
}

bl_err_t bl_frame_decode(const uint8_t *wire, size_t len, uint8_t *buf, size_t cap, bl_frame_t *f)
{
    size_t n;
    if (len > COBS_MAX_ENCODED(BL_FRAME_MAX)) {
        return BL_ERR_TOO_LONG;
    }
    if (!cobs_decode(wire, len, buf, cap, &n)) {
        return len > cap ? BL_ERR_TOO_LONG : BL_ERR_COBS;
    }
    if (n < 3) {
        return BL_ERR_SHORT;
    }
    if (n > BL_FRAME_MAX) {
        return BL_ERR_TOO_LONG;
    }
    uint16_t want = (uint16_t)(buf[n - 2] | (buf[n - 1] << 8));
    if (crc16_ccitt_false(buf, n - 2) != want) {
        return BL_ERR_CRC;
    }
    if (buf[0] < BL_PUBLISH || buf[0] > BL_ACK) {
        return BL_ERR_TYPE;
    }
    f->type = buf[0];
    f->body = buf + 1;
    f->body_len = n - 3;
    return BL_OK;
}

void bl_deframer_init(bl_deframer_t *d, uint8_t *buf, size_t cap)
{
    d->buf = buf;
    d->cap = cap;
    d->len = 0;
    d->discarding = false;
    d->overruns = 0;
}

bool bl_deframer_push(bl_deframer_t *d, uint8_t byte, const uint8_t **frame, size_t *len)
{
    if (byte == 0x00) {
        bool done = !d->discarding && d->len > 0;
        if (done) {
            *frame = d->buf;
            *len = d->len;
        }
        d->len = 0;
        d->discarding = false;
        return done;
    }
    if (d->discarding) {
        return false;
    }
    if (d->len >= d->cap) {
        d->discarding = true;
        d->overruns++;
        return false;
    }
    d->buf[d->len++] = byte;
    return false;
}

/* ------------------------------------------------------------------ bodies */

static bool topic_ok(const char *t, size_t n)
{
    if (n < 1 || n > BL_TOPIC_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (t[i] < 0x21 || t[i] > 0x7E || t[i] == '+' || t[i] == '#') {
            return false;
        }
    }
    return true;
}

static bool qos_seq_ok(uint8_t qos, uint8_t seq)
{
    return (qos == 0 && seq == 0) || (qos == 1 && seq != 0);
}

size_t bl_relay_encode(const bl_relay_t *r, uint8_t *out, size_t cap)
{
    size_t n = 3 + r->topic_len + r->payload_len;
    if (!topic_ok(r->topic, r->topic_len) || !qos_seq_ok(r->qos, r->seq) || n > BL_BODY_MAX || n > cap) {
        return 0;
    }
    out[0] = (uint8_t)((r->retain ? 0x01 : 0) | (r->qos << 1));
    out[1] = r->seq;
    out[2] = (uint8_t)r->topic_len;
    memcpy(out + 3, r->topic, r->topic_len);
    if (r->payload_len) {
        memcpy(out + 3 + r->topic_len, r->payload, r->payload_len);
    }
    return n;
}

bl_err_t bl_relay_parse(const uint8_t *body, size_t len, bl_relay_t *r)
{
    if (len < 3 || len < 3 + (size_t)body[2]) {
        return BL_ERR_BODY;
    }
    r->retain = body[0] & 0x01;
    r->qos = (body[0] >> 1) & 0x03;
    r->seq = body[1];
    r->topic_len = body[2];
    r->topic = (const char *)body + 3;
    r->payload = body + 3 + r->topic_len;
    r->payload_len = len - 3 - r->topic_len;
    if (!topic_ok(r->topic, r->topic_len) || !qos_seq_ok(r->qos, r->seq)) {
        return BL_ERR_BODY;
    }
    return BL_OK;
}

size_t bl_subscribe_encode(uint8_t seq, const bl_sub_entry_t *e, size_t n, uint8_t *out, size_t cap)
{
    if (seq == 0 || n == 0 || cap < 1) {
        return 0;
    }
    size_t pos = 0;
    out[pos++] = seq;
    for (size_t i = 0; i < n; i++) {
        if (e[i].qos > 1 || !topic_ok(e[i].topic, e[i].topic_len) || pos + 2 + e[i].topic_len > cap ||
                pos + 2 + e[i].topic_len > BL_BODY_MAX) {
            return 0;
        }
        out[pos++] = e[i].qos;
        out[pos++] = (uint8_t)e[i].topic_len;
        memcpy(out + pos, e[i].topic, e[i].topic_len);
        pos += e[i].topic_len;
    }
    return pos;
}

bl_err_t bl_subscribe_parse(const uint8_t *body, size_t len, uint8_t *seq, bl_sub_entry_t *e, size_t max,
                            size_t *n)
{
    if (len < 1 || body[0] == 0) {
        return BL_ERR_BODY;
    }
    *seq = body[0];
    size_t pos = 1, count = 0;
    while (pos < len) {
        if (len - pos < 2 || count >= max) {
            return BL_ERR_BODY;
        }
        uint8_t qos = body[pos], tl = body[pos + 1];
        const char *t = (const char *)body + pos + 2;
        if (qos > 1 || len - pos - 2 < tl || !topic_ok(t, tl)) {
            return BL_ERR_BODY;
        }
        e[count].qos = qos;
        e[count].topic = t;
        e[count].topic_len = tl;
        count++;
        pos += 2 + (size_t)tl;
    }
    if (count == 0) {
        return BL_ERR_BODY;
    }
    *n = count;
    return BL_OK;
}

bl_err_t bl_ack_parse(const uint8_t *body, size_t len, uint8_t *seq)
{
    if (len != 1 || body[0] == 0) {
        return BL_ERR_BODY;
    }
    *seq = body[0];
    return BL_OK;
}

size_t bl_hb_gw_encode(const bl_hb_gw_t *h, uint8_t *out, size_t cap)
{
    if (cap < BL_HB_GW_LEN || h->ms > 999) {
        return 0;
    }
    out[0] = h->version;
    out[1] = h->flags;
    for (int i = 0; i < 4; i++) {
        out[2 + i] = (uint8_t)(h->unix_s >> (8 * i));
    }
    out[6] = (uint8_t)(h->ms & 0xFF);
    out[7] = (uint8_t)(h->ms >> 8);
    return BL_HB_GW_LEN;
}

bl_err_t bl_hb_gw_parse(const uint8_t *body, size_t len, bl_hb_gw_t *h)
{
    if (len < BL_HB_GW_LEN) {
        return BL_ERR_BODY;
    }
    h->version = body[0];
    h->flags = body[1];
    h->unix_s = (uint32_t)body[2] | ((uint32_t)body[3] << 8) | ((uint32_t)body[4] << 16) | ((uint32_t)body[5] << 24);
    h->ms = (uint16_t)(body[6] | (body[7] << 8));
    return BL_OK;
}

size_t bl_hb_ap_encode(uint8_t version, uint8_t *out, size_t cap)
{
    if (cap < BL_HB_AP_LEN) {
        return 0;
    }
    out[0] = version;
    out[1] = 0;
    return BL_HB_AP_LEN;
}

bl_err_t bl_hb_ap_parse(const uint8_t *body, size_t len, uint8_t *version)
{
    if (len < BL_HB_AP_LEN) {
        return BL_ERR_BODY;
    }
    *version = body[0];
    return BL_OK;
}
