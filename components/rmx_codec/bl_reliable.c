#include "bl_reliable.h"

void bl_rel_tx_init(bl_rel_tx_t *t)
{
    t->next_seq = 1;
    t->seq = 0;
    t->busy = false;
    t->resends = 0;
    t->sent_ms = 0;
    t->dropped = 0;
}

bool bl_rel_tx_busy(const bl_rel_tx_t *t)
{
    return t->busy;
}

uint8_t bl_rel_tx_start(bl_rel_tx_t *t, uint32_t now_ms)
{
    if (t->busy) {
        return 0;
    }
    t->seq = t->next_seq;
    t->next_seq = t->next_seq == 255 ? 1 : (uint8_t)(t->next_seq + 1);
    t->busy = true;
    t->resends = 0;
    t->sent_ms = now_ms;
    return t->seq;
}

bl_rel_action_t bl_rel_tx_poll(bl_rel_tx_t *t, uint32_t now_ms)
{
    if (!t->busy || (uint32_t)(now_ms - t->sent_ms) < BL_REL_TIMEOUT_MS) {
        return BL_REL_NONE;
    }
    if (t->resends >= BL_REL_RESENDS) {
        t->busy = false;
        t->dropped++;
        return BL_REL_DROPPED;
    }
    t->resends++;
    t->sent_ms = now_ms;
    return BL_REL_RESEND;
}

bool bl_rel_tx_ack(bl_rel_tx_t *t, uint8_t seq)
{
    if (!t->busy || seq != t->seq) {
        return false;   /* late ACK of an earlier frame, or garbage */
    }
    t->busy = false;
    return true;
}

void bl_rel_tx_abort(bl_rel_tx_t *t)
{
    t->busy = false;
}

void bl_rel_rx_init(bl_rel_rx_t *r)
{
    r->last = 0;
    r->have_boot = false;
    r->peer_boot = 0;
}

bool bl_rel_rx_accept(bl_rel_rx_t *r, uint8_t seq)
{
    if (seq == 0 || seq == r->last) {
        return false;
    }
    r->last = seq;
    return true;
}

void bl_rel_rx_reset(bl_rel_rx_t *r)
{
    r->last = 0;
}

bool bl_rel_rx_peer_boot(bl_rel_rx_t *r, uint8_t boot)
{
    bool restarted = r->have_boot && boot != r->peer_boot;
    r->have_boot = true;
    r->peer_boot = boot;
    if (restarted) {
        r->last = 0;
    }
    return restarted;
}
