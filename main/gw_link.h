/* Link state of the gateway in one place (wiki Mower-Gateway-Architecture §4.1):
 * level bits in one event group, and a GW_EVENT on the default event loop for
 * every bit that changes (event id = bit index, data = bool level).
 *
 * The owners set their bits; gw_link derives SERVER_LINK_UP (MQTT up and the
 * retained mower/status/server "online", Mower-Protocol §4.2) and SERVER_LOST
 * (that link down for 10 s without a break, also counted from boot). */
#pragma once

#include <stdbool.h>
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define GW_CELL_UP         BIT0   /* PPP has an IP (cell) */
#define GW_MQTT_UP         BIT1   /* MQTT connected (mqtt_link) */
#define GW_SERVER_ONLINE   BIT2   /* retained mower/status/server is "online" (mqtt_link) */
#define GW_SERVER_LINK_UP  BIT3   /* derived: MQTT_UP and SERVER_ONLINE */
#define GW_SERVER_LOST     BIT4   /* derived: SERVER_LINK_UP clear for 10 s */
#define GW_CLOCK_VALID     BIT5   /* SNTP set the clock (app_main) */
#define GW_BL_UP           BIT6   /* reserved: board link (#62) */
#define GW_NTRIP_STREAM    BIT7   /* reserved: NTRIP corrections streaming (#63) */

ESP_EVENT_DECLARE_BASE(GW_EVENT);

/* After esp_event_loop_create_default(), before any module sets a bit. */
esp_err_t gw_link_init(void);

/* Sets or clears an owner bit (not the derived ones). Callable from any task,
 * not from an ISR. Posts GW_EVENT for each bit that changed. */
void gw_link_set(EventBits_t bit, bool on);

EventBits_t gw_link_get(void);

/* Waits until all of the bits are set; false on timeout. */
bool gw_link_wait(EventBits_t all, int timeout_ms);
