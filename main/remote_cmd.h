/* Gateway commands (wiki Mower-Protocol §5, §6.4): mower/<name>/gateway/cmd in,
 * gateway/cmd/ack out.
 *
 * Command: {"id":"c1","cmd":"ping","expires_at":1794650060,...}
 *   cmd: ping | restart | ota | monitor_start | monitor_stop
 *   (ntrip_set, ntrip_clear and autopilot_reset belong to the autopilot and
 *   NTRIP issues; ntrip_* answer unknown_cmd, autopilot_reset "failed")
 * Ack: {"id","cmd","ok":true,"state":"accepted|done","result":{...},"time":1794650001.1}
 *      {"id","cmd","ok":false,"state":"failed","error":"<code>","message":"...","time":...}
 * Every command gets one final ack (done or failed); ota first gets accepted.
 * Command ids are remembered for 3 minutes with their last ack; the same id
 * again repeats that ack and does not run the command again.
 */
#pragma once

#include "cJSON.h"
#include "esp_err.h"

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
