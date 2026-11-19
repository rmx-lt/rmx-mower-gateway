/* Monitor streams (wiki Mower-Protocol §15): short fast samples of chosen
 * metrics, on gateway/monitor, within a budget of 100 values per second. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

/* Before the first monitor_start or monitor_tick. */
esp_err_t monitor_init(void);

/* monitor_start (§15). result always gets {"budget","used"}. Returns false with
 * an error code (bad_request, over_budget) and message when refused. */
bool monitor_start(const cJSON *cmd, cJSON *result, const char **error, const char **message);

/* monitor_stop (§15): the named session, or every stream without "session".
 * Always succeeds. */
void monitor_stop(const cJSON *cmd, cJSON *result);

/* Samples and publishes the due streams; ends them when the hold runs out or
 * the server link is lost. Called by the metrics tick every 100 ms. */
void monitor_tick(int64_t now);
