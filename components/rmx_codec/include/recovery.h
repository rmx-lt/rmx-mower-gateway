/* The decisions of the supervisor's recovery ladder and task liveness (wiki
 * Mower-Gateway-Architecture §4, issue #79). Pure: the caller passes the link
 * state and the time, and carries out the action. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t mqtt_down_ms;   /* MQTT down with PPP up this long: reconnect the cell (5 min) */
    int64_t link_down_ms;   /* server link down this long: restart (30 min) */
} rec_cfg_t;

typedef enum {
    REC_NONE,
    REC_CELL_RECONNECT,
    REC_RESTART,
} rec_action_t;

/* Zero-initialise; the first rec_step starts the clocks. */
typedef struct {
    bool started;
    int64_t mqtt_since_ms;   /* when "MQTT down with PPP up" began, or -1 */
    int64_t link_since_ms;   /* when the server link went down */
} rec_state_t;

/* One supervisor tick. While an OTA writes, nothing happens and both clocks
 * start over afterwards. A cell reconnect repeats every mqtt_down_ms while the
 * condition lasts. *reason is set when the action is not REC_NONE. */
rec_action_t rec_step(rec_state_t *st, const rec_cfg_t *cfg, int64_t now_ms, bool ppp_up, bool mqtt_up,
                      bool link_up, bool ota_writing, const char **reason);

/* One watched task: its loop counter as last seen, and when it last changed.
 * Zero-initialise. */
typedef struct {
    uint32_t last;
    int64_t changed_ms;
    bool seen;
} live_t;

/* Compares counters[i] with tasks[i] for n tasks. Returns the index of the
 * first task whose counter has not changed for stale_ms, else -1. While an OTA
 * writes every task counts as alive. */
int live_check(live_t *tasks, const uint32_t *counters, int n, int64_t now_ms, int64_t stale_ms, bool ota_writing);
