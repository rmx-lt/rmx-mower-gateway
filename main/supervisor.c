#include "supervisor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cell.h"
#include "gw_link.h"
#include "gwstate.h"
#include "mqtt_link.h"
#include "ota.h"

static const char *TAG = "supervisor";

#define ALIVE_US (60 * 1000000LL)

static TaskHandle_t s_task;

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
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));   /* a link event, or once a second */
        if (mqtt_link_wait_ready(0)) {
            ota_confirm();   /* a new image that reaches the broker is good; cheap once confirmed */
        }
        bool up = gw_link_get() & GW_SERVER_LINK_UP;
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
        if (now - alive_us >= ALIVE_US) {
            alive_us = now;
            ESP_LOGI(TAG, "alive: ppp %s, server link %s, rssi %d, free heap %u", cell_is_up() ? "up" : "down",
                     up ? "up" : "down", cell_last_rssi(), (unsigned)esp_get_free_heap_size());
        }
    }
}

esp_err_t supervisor_start(void)
{
    if (xTaskCreate(supervisor_task, "supervisor", 6144, NULL, 3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return esp_event_handler_register(GW_EVENT, ESP_EVENT_ANY_ID, on_link_event, NULL);
}
