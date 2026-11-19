/* Consistent Overhead Byte Stuffing (wiki Mower-Protocol §14.1). The encoded
 * bytes contain no 0x00; the link adds one 0x00 after each frame. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Worst case encoded size of n input bytes (without the 0x00 delimiter). */
#define COBS_MAX_ENCODED(n) ((n) + (n) / 254 + 1)

/* Incremental encoder: put bytes one by one, then end. */
typedef struct {
    uint8_t *out;
    size_t cap;
    size_t pos;        /* next free byte */
    size_t code_pos;   /* where the current block's code byte goes */
    uint8_t code;
    bool full;         /* the current block reached 254 data bytes */
    bool overflow;
} cobs_enc_t;

void cobs_enc_begin(cobs_enc_t *e, uint8_t *out, size_t cap);
void cobs_enc_put(cobs_enc_t *e, uint8_t b);
/* Encoded length, or 0 when out was too small (a valid encoding is >= 1 byte). */
size_t cobs_enc_end(cobs_enc_t *e);

size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t cap);

/* Decodes len bytes (without the delimiter). False on an empty input, a 0x00
 * inside, a block running past the end, or out too small. */
bool cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *out_len);
