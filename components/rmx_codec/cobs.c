#include "cobs.h"

static void emit(cobs_enc_t *e, size_t at, uint8_t b)
{
    if (at < e->cap) {
        e->out[at] = b;
    } else {
        e->overflow = true;
    }
}

/* Closes the current block and opens the next one. */
static void next_block(cobs_enc_t *e)
{
    emit(e, e->code_pos, e->code);
    e->code_pos = e->pos++;
    e->code = 1;
    e->full = false;
}

void cobs_enc_begin(cobs_enc_t *e, uint8_t *out, size_t cap)
{
    e->out = out;
    e->cap = cap;
    e->code_pos = 0;
    e->pos = 1;
    e->code = 1;
    e->full = false;
    e->overflow = false;
}

void cobs_enc_put(cobs_enc_t *e, uint8_t b)
{
    if (e->full) {
        next_block(e);   /* only now: a full block at the very end needs no extra code byte */
    }
    if (b == 0) {
        next_block(e);
        return;
    }
    emit(e, e->pos++, b);
    if (++e->code == 0xFF) {
        e->full = true;
    }
}

size_t cobs_enc_end(cobs_enc_t *e)
{
    emit(e, e->code_pos, e->code);
    return e->overflow ? 0 : e->pos;
}

size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    cobs_enc_t e;
    cobs_enc_begin(&e, out, cap);
    for (size_t i = 0; i < len; i++) {
        cobs_enc_put(&e, in[i]);
    }
    return cobs_enc_end(&e);
}

bool cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t i = 0, n = 0;
    if (len == 0) {
        return false;
    }
    while (i < len) {
        uint8_t code = in[i++];
        if (code == 0) {
            return false;
        }
        for (uint8_t k = 1; k < code; k++) {
            if (i >= len || in[i] == 0 || n >= cap) {
                return false;
            }
            out[n++] = in[i++];
        }
        if (code != 0xFF && i < len) {
            if (n >= cap) {
                return false;
            }
            out[n++] = 0;
        }
    }
    *out_len = n;
    return true;
}
