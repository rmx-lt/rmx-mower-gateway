#include "cmd_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool cmd_id_valid(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n < 1 || n > CMD_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool cmd_name_valid(const char *name)
{
    return name && name[0] && strlen(name) <= CMD_NAME_MAX;
}

static cmd_mem_slot_t *find(cmd_mem_t *mem, const char *id)
{
    for (int i = 0; i < CMD_MEM_IDS; i++) {
        if (mem->slot[i].ack && strcmp(mem->slot[i].id, id) == 0) {
            return &mem->slot[i];
        }
    }
    return NULL;
}

const char *cmd_mem_find(cmd_mem_t *mem, const char *id, int64_t now_us)
{
    cmd_mem_slot_t *s = id ? find(mem, id) : NULL;
    if (!s) {
        return NULL;
    }
    s->t_us = now_us;
    return s->ack;
}

/* A free slot, else an expired finished one, else the oldest finished one,
 * else the oldest. The slot comes back empty. */
static cmd_mem_slot_t *take_slot(cmd_mem_t *mem, int64_t now_us)
{
    cmd_mem_slot_t *oldest_done = NULL;
    cmd_mem_slot_t *oldest = NULL;
    cmd_mem_slot_t *pick = NULL;
    for (int i = 0; i < CMD_MEM_IDS && !pick; i++) {
        cmd_mem_slot_t *s = &mem->slot[i];
        if (!s->ack || (!s->open && now_us - s->t_us > CMD_MEM_US)) {
            pick = s;
        } else {
            if (!s->open && (!oldest_done || s->t_us < oldest_done->t_us)) {
                oldest_done = s;
            }
            if (!oldest || s->t_us < oldest->t_us) {
                oldest = s;
            }
        }
    }
    if (!pick) {
        pick = oldest_done ? oldest_done : oldest;
    }
    free(pick->ack);
    pick->ack = NULL;
    return pick;
}

bool cmd_mem_store(cmd_mem_t *mem, const char *id, const char *ack, bool open, int64_t now_us)
{
    if (!cmd_id_valid(id) || !ack) {
        return false;
    }
    size_t n = strlen(ack) + 1;
    char *copy = malloc(n);   /* not strdup: strict C11 on the CI host */
    if (!copy) {
        return false;
    }
    memcpy(copy, ack, n);
    cmd_mem_slot_t *s = find(mem, id);
    if (s) {
        free(s->ack);
    } else {
        s = take_slot(mem, now_us);
        snprintf(s->id, sizeof(s->id), "%s", id);
    }
    s->ack = copy;
    s->open = open;
    s->t_us = now_us;
    return true;
}

void cmd_mem_clear(cmd_mem_t *mem)
{
    for (int i = 0; i < CMD_MEM_IDS; i++) {
        free(mem->slot[i].ack);
    }
    memset(mem, 0, sizeof(*mem));
}

const char *cmd_check_expires(cmd_expires_t kind, double expires_at, double now, const char **message)
{
    switch (kind) {
    case CMD_EXPIRES_ABSENT:
        return NULL;
    case CMD_EXPIRES_OTHER:
        *message = "expires_at must be a unix time in seconds";
        return CMD_E_BAD_REQUEST;
    default:
        break;
    }
    if (now < 0) {
        *message = "the device clock is not set, cannot check expires_at";
        return CMD_E_NO_CLOCK;
    }
    if (now > expires_at) {
        *message = "command expired before it arrived";
        return CMD_E_EXPIRED;
    }
    return NULL;
}
