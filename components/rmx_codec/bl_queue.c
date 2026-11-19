#include "bl_queue.h"

#include <string.h>

/* Entry: u32 time (ms), u32 data length, data; may wrap around the ring end. */

static void ring_write(bl_queue_t *q, size_t at, const uint8_t *src, size_t n)
{
    if (n == 0) {
        return;
    }
    at %= q->cap;
    size_t first = q->cap - at < n ? q->cap - at : n;
    memcpy(q->buf + at, src, first);
    memcpy(q->buf, src + first, n - first);
}

static void ring_read(const bl_queue_t *q, size_t at, uint8_t *dst, size_t n)
{
    if (n == 0) {
        return;
    }
    at %= q->cap;
    size_t first = q->cap - at < n ? q->cap - at : n;
    memcpy(dst, q->buf + at, first);
    memcpy(dst + first, q->buf, n - first);
}

static void read_hdr(const bl_queue_t *q, uint32_t *t, uint32_t *len)
{
    uint8_t h[BL_QUEUE_ENTRY_OVERHEAD];
    ring_read(q, q->head, h, sizeof(h));
    memcpy(t, h, 4);
    memcpy(len, h + 4, 4);
}

void bl_queue_init(bl_queue_t *q, uint8_t *buf, size_t cap, uint32_t max_age_ms)
{
    q->buf = buf;
    q->cap = cap;
    q->head = 0;
    q->used = 0;
    q->count = 0;
    q->max_age_ms = max_age_ms;
    q->dropped_full = 0;
    q->dropped_age = 0;
}

void bl_queue_drop(bl_queue_t *q)
{
    if (q->count == 0) {
        return;
    }
    uint32_t t, len;
    read_hdr(q, &t, &len);
    size_t n = BL_QUEUE_ENTRY_OVERHEAD + len;
    q->head = (q->head + n) % q->cap;
    q->used -= n;
    q->count--;
}

void bl_queue_expire(bl_queue_t *q, uint32_t now_ms)
{
    while (q->count > 0) {
        uint32_t t, len;
        read_hdr(q, &t, &len);
        if ((uint32_t)(now_ms - t) < q->max_age_ms) {
            return;   /* entries are in time order: the rest are younger */
        }
        bl_queue_drop(q);
        q->dropped_age++;
    }
}

bool bl_queue_push(bl_queue_t *q, uint32_t now_ms, const void *data, size_t len)
{
    bl_queue_expire(q, now_ms);
    if (q->cap < BL_QUEUE_ENTRY_OVERHEAD || len > q->cap - BL_QUEUE_ENTRY_OVERHEAD) {
        q->dropped_full++;
        return false;
    }
    size_t n = BL_QUEUE_ENTRY_OVERHEAD + len;
    while (q->cap - q->used < n) {
        bl_queue_drop(q);
        q->dropped_full++;
    }
    uint8_t h[BL_QUEUE_ENTRY_OVERHEAD];
    uint32_t len32 = (uint32_t)len;
    memcpy(h, &now_ms, 4);
    memcpy(h + 4, &len32, 4);
    size_t tail = q->head + q->used;
    ring_write(q, tail, h, sizeof(h));
    ring_write(q, tail + sizeof(h), data, len);
    q->used += n;
    q->count++;
    return true;
}

bool bl_queue_peek(bl_queue_t *q, uint32_t now_ms, void *out, size_t cap, size_t *len)
{
    bl_queue_expire(q, now_ms);
    if (q->count == 0) {
        return false;
    }
    uint32_t t, n;
    read_hdr(q, &t, &n);
    *len = n;
    if (n > cap) {
        return false;
    }
    ring_read(q, q->head + BL_QUEUE_ENTRY_OVERHEAD, out, n);
    return true;
}

uint32_t bl_queue_count(const bl_queue_t *q)
{
    return q->count;
}
