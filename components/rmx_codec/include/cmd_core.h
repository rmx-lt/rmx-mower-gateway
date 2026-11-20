/* The pure rules of gateway commands (wiki Mower-Protocol §5, §6.4): command
 * ids, the memory of recent ids and their last ack, the expires_at check, and
 * the protocol's error codes. No locking: the caller serialises access. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Error codes of a failed ack (§5). */
#define CMD_E_BAD_REQUEST    "bad_request"
#define CMD_E_NO_CLOCK       "no_clock"
#define CMD_E_EXPIRED        "expired"
#define CMD_E_BUSY           "busy"
#define CMD_E_UNKNOWN_CMD    "unknown_cmd"
#define CMD_E_FAILED         "failed"
#define CMD_E_OVER_BUDGET    "over_budget"
#define CMD_E_SAME_VERSION   "same_version"
#define CMD_E_LOW_SIGNAL     "low_signal"
#define CMD_E_DOWNLOAD       "download_failed"
#define CMD_E_BAD_IMAGE      "bad_image"
#define CMD_E_BAD_SIZE       "bad_size"

#define CMD_ID_MAX   64
#define CMD_NAME_MAX 32
#define CMD_MEM_IDS  16
#define CMD_MEM_US   (180 * 1000000LL)   /* the contract asks for at least 2 minutes */

/* 1-64 chars of A-Za-z0-9_- */
bool cmd_id_valid(const char *id);
/* 1-32 chars */
bool cmd_name_valid(const char *name);

/* The ids seen recently, each with the last ack sent for it. Zero-initialise. */
typedef struct {
    char id[CMD_ID_MAX + 1];
    char *ack;      /* JSON, malloc'd; NULL = slot free */
    bool open;      /* the last ack was "accepted": the command still runs */
    int64_t t_us;   /* last store or find */
} cmd_mem_slot_t;

typedef struct {
    cmd_mem_slot_t slot[CMD_MEM_IDS];
} cmd_mem_t;

/* The last ack stored for id, or NULL. Refreshes its time. The pointer stays
 * valid until the next cmd_mem_store or cmd_mem_clear. */
const char *cmd_mem_find(cmd_mem_t *mem, const char *id, int64_t now_us);

/* Remembers ack as the last one for id (copied). A new id takes a free slot,
 * else a finished one older than CMD_MEM_US, else the oldest finished one, else
 * the oldest. False for an invalid id or no memory. */
bool cmd_mem_store(cmd_mem_t *mem, const char *id, const char *ack, bool open, int64_t now_us);

/* Frees every slot. */
void cmd_mem_clear(cmd_mem_t *mem);

/* What the command carried as expires_at. */
typedef enum {
    CMD_EXPIRES_ABSENT,
    CMD_EXPIRES_NUMBER,
    CMD_EXPIRES_OTHER,   /* present, not a number */
} cmd_expires_t;

/* The expires_at rule (§5). now is unix time in seconds, negative while the
 * clock is not set. NULL when the command may run, else the error code with
 * *message set. */
const char *cmd_check_expires(cmd_expires_t kind, double expires_at, double now, const char **message);
