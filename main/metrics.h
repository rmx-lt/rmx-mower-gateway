/* The gateway's metric catalogue, telemetry and monitor streams (wiki
 * Mower-Protocol §6.3, §15, §16), and the task that publishes state, catalogue
 * and telemetry when the server link comes up. */
#pragma once

#include <stdbool.h>
#include "cJSON.h"

/* Starts the publisher task (100 ms tick: server link, state, telemetry,
 * monitor streams) and the task that refreshes the signal reading. Call after
 * the data connection is up, before or after mqtt_link_start(). */
void metrics_start(void);

/* monitor_start (§15). result always gets {"budget","used"}. Returns false with
 * an error code (bad_request, over_budget) and message when refused. */
bool monitor_start(const cJSON *cmd, cJSON *result, const char **error, const char **message);

/* monitor_stop (§15): the named session, or every stream without "session".
 * Always succeeds. */
void monitor_stop(const cJSON *cmd, cJSON *result);
