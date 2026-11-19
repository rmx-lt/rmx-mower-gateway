/* Offline queue of the board link (wiki Mower-Protocol §14.2): reliable
 * messages from the autopilot kept while MQTT is down, for up to 60 s and
 * 64 KB; beyond either limit the oldest are dropped and counted.
 *
 * FIFO of variable-size entries in a caller-provided byte ring. Each entry
 * costs BL_QUEUE_ENTRY_OVERHEAD bytes plus its data, and the 64 KB limit is
 * the buffer size, overhead included. No RTOS, no heap; the caller passes
 * the clock in milliseconds (wrapping uint32 is fine) and locks if needed. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BL_QUEUE_BYTES           (64 * 1024)
#define BL_QUEUE_MAX_AGE_MS      60000
#define BL_QUEUE_ENTRY_OVERHEAD  8   /* time stamp + length */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t head;          /* oldest entry */
    size_t used;          /* bytes in use, overhead included */
    uint32_t count;
    uint32_t max_age_ms;
    uint32_t dropped_full;
    uint32_t dropped_age;
} bl_queue_t;

void bl_queue_init(bl_queue_t *q, uint8_t *buf, size_t cap, uint32_t max_age_ms);
/* Adds a copy of data, dropping the oldest entries until it fits. False (and
 * counted in dropped_full) when it can never fit: len + overhead > cap. */
bool bl_queue_push(bl_queue_t *q, uint32_t now_ms, const void *data, size_t len);
/* Drops entries older than max_age_ms (age >= max_age_ms). */
void bl_queue_expire(bl_queue_t *q, uint32_t now_ms);
/* Copies the oldest entry (after expiring) without removing it. False when
 * empty, or when it is larger than cap (then *len is its size). */
bool bl_queue_peek(bl_queue_t *q, uint32_t now_ms, void *out, size_t cap, size_t *len);
/* Removes the oldest entry, e.g. after it was published. */
void bl_queue_drop(bl_queue_t *q);
uint32_t bl_queue_count(const bl_queue_t *q);
