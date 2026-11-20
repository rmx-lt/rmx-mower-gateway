#include "gw_link.h"

#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "link";

ESP_EVENT_DEFINE_BASE(GW_EVENT);

#define SERVER_LOST_US (10 * 1000000LL)   /* wiki Mower-Protocol §4.2 */
#define OWNER_BITS (GW_CELL_UP | GW_MQTT_UP | GW_SERVER_ONLINE | GW_CLOCK_VALID | GW_BL_UP | GW_NTRIP_STREAM)

static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_lost_timer;
static EventBits_t s_bits;   /* the truth, under s_lock; s_ev mirrors it */

static const char *bit_name(int idx)
{
    static const char *const names[] = { "cell", "mqtt", "server_online", "server_link", "server_lost",
                                         "clock", "board_link", "ntrip" };
    return idx < (int)(sizeof(names) / sizeof(names[0])) ? names[idx] : "?";
}

/* Caller holds s_lock. Applies new bits, mirrors them and posts the changes. */
static void apply(EventBits_t next)
{
    if ((next & GW_MQTT_UP) && (next & GW_SERVER_ONLINE)) {
        next |= GW_SERVER_LINK_UP;
    } else {
        next &= ~GW_SERVER_LINK_UP;
    }
    if (next & GW_SERVER_LINK_UP) {
        next &= ~GW_SERVER_LOST;
    }
    EventBits_t changed = s_bits ^ next;
    if (!changed) {
        return;
    }
    if (changed & GW_SERVER_LINK_UP) {
        if (next & GW_SERVER_LINK_UP) {
            esp_timer_stop(s_lost_timer);
        } else {
            esp_timer_start_once(s_lost_timer, SERVER_LOST_US);
        }
    }
    s_bits = next;
    xEventGroupSetBits(s_ev, next);
    xEventGroupClearBits(s_ev, ~next & 0x00FFFFFF);
    for (int i = 0; i < 8; i++) {
        if (changed & (1u << i)) {
            bool level = next & (1u << i);
            ESP_LOGI(TAG, "%s %s", bit_name(i), level ? "up" : "down");
            esp_event_post(GW_EVENT, i, &level, sizeof(level), 0);   /* a full queue only delays a waiter's 1 s poll */
        }
    }
}

static void on_lost_timer(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!(s_bits & GW_SERVER_LINK_UP)) {
        apply(s_bits | GW_SERVER_LOST);
    }
    xSemaphoreGive(s_lock);
}

esp_err_t gw_link_init(void)
{
    s_ev = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t args = { .callback = on_lost_timer, .name = "server_lost" };
    if (!s_ev || !s_lock || esp_timer_create(&args, &s_lost_timer) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    /* The server link is down at boot: lost 10 s from now unless it comes up. */
    return esp_timer_start_once(s_lost_timer, SERVER_LOST_US);
}

void gw_link_set(EventBits_t bit, bool on)
{
    bit &= OWNER_BITS;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    apply(on ? (s_bits | bit) : (s_bits & ~bit));
    xSemaphoreGive(s_lock);
}

EventBits_t gw_link_get(void)
{
    return s_ev ? xEventGroupGetBits(s_ev) : 0;
}

bool gw_link_wait(EventBits_t all, int timeout_ms)
{
    return (xEventGroupWaitBits(s_ev, all, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & all) == all;
}
