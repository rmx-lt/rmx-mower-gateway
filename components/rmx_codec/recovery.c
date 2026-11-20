#include "recovery.h"

#include <stddef.h>

rec_action_t rec_step(rec_state_t *st, const rec_cfg_t *cfg, int64_t now_ms, bool ppp_up, bool mqtt_up,
                      bool link_up, bool ota_writing, const char **reason)
{
    if (!st->started || ota_writing) {
        st->started = true;
        st->mqtt_since_ms = -1;
        st->link_since_ms = now_ms;   /* down counts from boot, and from the end of an OTA */
        return REC_NONE;
    }
    if (link_up) {
        st->link_since_ms = now_ms;
    } else if (now_ms - st->link_since_ms >= cfg->link_down_ms) {
        st->link_since_ms = now_ms;
        *reason = "server link down too long";
        return REC_RESTART;
    }
    if (!ppp_up || mqtt_up) {
        st->mqtt_since_ms = -1;
    } else if (st->mqtt_since_ms < 0) {
        st->mqtt_since_ms = now_ms;
    } else if (now_ms - st->mqtt_since_ms >= cfg->mqtt_down_ms) {
        st->mqtt_since_ms = now_ms;   /* again after another period */
        *reason = "MQTT down with PPP up";
        return REC_CELL_RECONNECT;
    }
    return REC_NONE;
}

int live_check(live_t *tasks, const uint32_t *counters, int n, int64_t now_ms, int64_t stale_ms, bool ota_writing)
{
    int stale = -1;
    for (int i = 0; i < n; i++) {
        live_t *t = &tasks[i];
        if (!t->seen || ota_writing || counters[i] != t->last) {
            t->seen = true;
            t->last = counters[i];
            t->changed_ms = now_ms;
        } else if (stale < 0 && now_ms - t->changed_ms >= stale_ms) {
            stale = i;
        }
    }
    return stale;
}
