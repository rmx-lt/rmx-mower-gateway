/* Gateway commands (wiki Mower-Protocol §5, §6.4): mower/<name>/gateway/cmd in,
 * gateway/cmd/ack out.
 *
 * Command: {"id":"c1","cmd":"ping","expires_at":1794650060,...}
 * Ack: {"id","cmd","ok":true,"state":"accepted|done","result":{...},"time":1794650001.1}
 *      {"id","cmd","ok":false,"state":"failed","error":"<code>","message":"...","time":...}
 * Every command gets one final ack (done or failed); a long one first gets
 * accepted. Command ids are remembered for 3 minutes with their last ack; the
 * same id again repeats that ack and does not run the command again. The pure
 * rules (ids, memory, expires_at, error codes) are in rmx_codec's cmd_core.h.
 *
 * Modules register their commands (cmd_register); remote_cmd itself has ping,
 * restart and autopilot_reset (not wired yet: answers failed).
 */
#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#include "cmd_core.h"

/* What a command handler answers. Leave everything 0 for "done" without a
 * result. */
typedef struct {
    const char *error;     /* a CMD_E_* code: the command failed */
    const char *message;   /* with error */
    cJSON *result;         /* optional, for done or failed; the dispatcher owns it */
    bool accepted;         /* the handler sent the "accepted" ack itself and acks the end later */
    bool restart;          /* restart the board after the ack */
} cmd_reply_t;

/* Runs on the command task; may block briefly. cmd is the whole command object. */
typedef void (*cmd_fn_t)(const char *id, const cJSON *cmd, cmd_reply_t *reply);

#define CMD_DURING_OTA  0x1   /* also runs while a firmware update is running */
#define CMD_SECRET_ARGS 0x2   /* the arguments are never logged, not even at debug level */

/* Adds a command. Before the first command can arrive (before MQTT connects).
 * ESP_ERR_NO_MEM when the table (16) is full, ESP_ERR_INVALID_ARG for a bad or
 * repeated name. */
esp_err_t cmd_register(const char *name, cmd_fn_t fn, unsigned flags);

/* Creates the worker task and registers with mqtt_link. Call before
 * mqtt_link_start(). */
esp_err_t remote_cmd_init(void);

/* Sends and remembers an ack. state is "accepted", "done" or "failed"; error and
 * message are only for "failed". result (optional) is owned by the call.
 * Callable from any task. */
void cmd_ack(const char *id, const char *cmd, const char *state, const char *error,
             const char *message, cJSON *result);

/* Remembers an ack without sending it: for a command that was open across a
 * restart, so a resend of its id gets the "accepted" ack again. */
void cmd_remember_accepted(const char *id, const char *cmd);
