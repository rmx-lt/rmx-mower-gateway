/* Reliable frames on the board link (wiki Mower-Protocol §14.2), one direction
 * each. No RTOS: the caller passes a millisecond clock (wrapping uint32 is fine)
 * and does the sending.
 *
 * Sender: one reliable frame in flight; numbers 1..255, then 1 again; resend
 * after 200 ms without an ACK, up to 5 times; then drop and count. One counter
 * covers every reliable type (QoS 1 PUBLISH/MESSAGE and SUBSCRIBE).
 * Receiver: a frame with the same number as the last one taken is a repeat
 * (ACK it again, do not pass it on); any other number is new. It forgets the
 * last number when the peer restarts (the boot byte of its heartbeat changes,
 * bl_rel_rx_peer_boot) and when the link goes down (bl_rel_rx_reset).
 *
 * Contract for the task that owns the link (rmx-mower-gateway#62): after the
 * board starts, send a HEARTBEAT before the first reliable frame, so the peer
 * has seen the new boot byte before it gets a frame numbered from 1 again. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BL_REL_TIMEOUT_MS 200
#define BL_REL_RESENDS    5

typedef enum {
    BL_REL_NONE,      /* nothing to do */
    BL_REL_RESEND,    /* send the same frame again now */
    BL_REL_DROPPED,   /* no ACK after the last resend: the frame is given up (counted) */
} bl_rel_action_t;

typedef struct {
    uint8_t next_seq;
    uint8_t seq;          /* in flight, valid while busy */
    bool busy;
    uint8_t resends;
    uint32_t sent_ms;
    uint32_t dropped;     /* metric link.dropped (this part of it) */
} bl_rel_tx_t;

void bl_rel_tx_init(bl_rel_tx_t *t);
bool bl_rel_tx_busy(const bl_rel_tx_t *t);
/* Takes the next number for a new frame; the caller puts it in the body and
 * sends the frame. 0 while a frame is in flight (wait for ACK or drop). */
uint8_t bl_rel_tx_start(bl_rel_tx_t *t, uint32_t now_ms);
/* Call often (every few ms, or from a timer at the deadline). */
bl_rel_action_t bl_rel_tx_poll(bl_rel_tx_t *t, uint32_t now_ms);
/* An ACK arrived. True when it was for the frame in flight (now free). */
bool bl_rel_tx_ack(bl_rel_tx_t *t, uint8_t seq);
/* Gives up the frame in flight without counting it (e.g. the link went down). */
void bl_rel_tx_abort(bl_rel_tx_t *t);

typedef struct {
    uint8_t last;         /* 0 = nothing taken since init or reset */
    bool have_boot;       /* a heartbeat from the peer was seen since init */
    uint8_t peer_boot;    /* its boot byte */
} bl_rel_rx_t;

void bl_rel_rx_init(bl_rel_rx_t *r);
/* A reliable frame with number seq (1..255) arrived. True: new, pass it on.
 * False: a repeat (or seq 0). ACK it in both cases. */
bool bl_rel_rx_accept(bl_rel_rx_t *r, uint8_t seq);
/* Forget the last number: call when the link goes down (3 s without a heartbeat). */
void bl_rel_rx_reset(bl_rel_rx_t *r);
/* Call with the boot byte of every heartbeat from the peer (§14.4). True when
 * it differs from the last one seen: the peer restarted and the last number is
 * forgotten. The first heartbeat after init only records the value. */
bool bl_rel_rx_peer_boot(bl_rel_rx_t *r, uint8_t boot);
