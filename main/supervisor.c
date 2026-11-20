#include "supervisor.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cell.h"
#include "gw_link.h"
#include "gw_tasks.h"
#include "gwstate.h"
#include "mqtt_link.h"
#include "ota.h"
#include "recovery.h"
#include "sdkconfig.h"

static const char *TAG = "supervisor";

#define ALIVE_US (60 * 1000000LL)

static TaskHandle_t s_task;

static const rec_cfg_t s_rec_cfg = {
    .mqtt_down_ms = CONFIG_GATEWAY_REC_MQTT_DOWN_S * 1000LL,
    .link_down_ms = CONFIG_GATEWAY_REC_LINK_DOWN_S * 1000LL,
};

/* §4.3 recovery ladder and task liveness, once a tick. */
static void recover(rec_state_t *rec, int64_t now_ms)
{
    EventBits_t bits = gw_link_get();
    bool ota = ota_busy();
    const char *why = NULL;
    switch (rec_step(rec, &s_rec_cfg, now_ms, bits & GW_CELL_UP, bits & GW_MQTT_UP, bits & GW_SERVER_LINK_UP,
                     ota, &why)) {
    case REC_CELL_RECONNECT:
        ESP_LOGW(TAG, "%s for %d s: reconnecting the cell", why, CONFIG_GATEWAY_REC_MQTT_DOWN_S);
        cell_reconnect();
        break;
    case REC_RESTART:
        gw_restart(why, 0);
    case REC_NONE:
        break;
    }
    const char *stuck = gw_tasks_stale(now_ms, CONFIG_GATEWAY_LIVE_STALE_S * 1000LL, ota);
    if (stuck) {
        char buf[64];
        snprintf(buf, sizeof(buf), "task %s did not loop for %d s", stuck, CONFIG_GATEWAY_LIVE_STALE_S);
        gw_restart(buf, 0);
    }
}

/* Runs on the default event loop task: only wakes the supervisor. */
static void on_link_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    xTaskNotifyGive(s_task);
}

static void supervisor_task(void *arg)
{
    bool was_up = false;
    bool reported = false;
    int64_t alive_us = esp_timer_get_time();
    rec_state_t rec = { 0 };
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));   /* a link event, or once a second */
        bool ready = mqtt_link_wait_ready(0);
        if (ready) {
            ota_confirm();   /* a new image that reaches the broker is good; cheap once confirmed */
        }
        bool up = gw_link_get() & GW_SERVER_LINK_UP;
        if (up && !was_up && !ready) {
            up = false;   /* the first state waits for ota_confirm() above, which stores "previous" (#108) */
        }
        if (up && !was_up) {
            ESP_LOGI(TAG, "server link up: publishing state");
            gw_state_publish(true);   /* §4.2; metrics publishes the catalogue on the same edge */
            reported = false;
        } else if (up) {
            gw_state_publish(false);   /* only when a field changed */
        }
        if (up && !reported) {
            reported = ota_report_result();   /* the final ack of an update that restarted us */
        }
        was_up = up;

        int64_t now = esp_timer_get_time();
        recover(&rec, now / 1000);
        if (now - alive_us >= ALIVE_US) {
            alive_us = now;
            ESP_LOGI(TAG, "alive: ppp %s, server link %s, rssi %d, free heap %u", cell_is_up() ? "up" : "down",
                     up ? "up" : "down", cell_last_rssi(), (unsigned)esp_get_free_heap_size());
            gw_tasks_log_stacks();
        }
    }
}

void gw_restart(const char *reason, int delay_ms)
{
    ESP_LOGW(TAG, "restarting in %d ms: %s", delay_ms, reason);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    mqtt_link_stop();
    cell_stop();
    esp_restart();
}

esp_err_t supervisor_start(void)
{
    if (gw_task_create(GW_T_SUPERVISOR, supervisor_task, NULL, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return esp_event_handler_register(GW_EVENT, ESP_EVENT_ANY_ID, on_link_event, NULL);
}
