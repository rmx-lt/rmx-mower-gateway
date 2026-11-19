/* MQTT over TLS to Mosquitto: the gateway's topics of wiki Mower-Protocol §3
 * (mower/<name>/gateway/...), commands, firmware chunks and the server link. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Where a subtopic lives: mower/<name>/gateway/<sub>, mower/<name>/autopilot/<sub>
 * (the board link relay, Mower-Protocol §14.2), or <sub> as the whole topic. */
typedef enum {
    MQTT_SCOPE_GATEWAY,
    MQTT_SCOPE_AUTOPILOT,
    MQTT_SCOPE_ABSOLUTE,
} mqtt_scope_t;

/* A message on a routed topic, after reassembly. Runs on the MQTT task: must not
 * block, hand the work to another task. subtopic is the route's own string. */
typedef void (*mqtt_route_cb_t)(const char *subtopic, const uint8_t *data, size_t len, void *ctx);

/* The route counts for mqtt_link_wait_ready(): ready only after its SUBACK. */
#define MQTT_ROUTE_READY 0x1

/* Subscribes scope/subtopic (no wildcards) on every connect, and at once when
 * connected. Callable before mqtt_link_start() and from any task. The same
 * scope and subtopic again replaces the handler. ESP_ERR_NO_MEM when the table
 * (24 routes) is full. */
esp_err_t mqtt_link_route(mqtt_scope_t scope, const char *subtopic, int qos, unsigned flags,
                          mqtt_route_cb_t cb, void *ctx);
/* Unsubscribes and forgets the route; ESP_ERR_NOT_FOUND if there was none. */
esp_err_t mqtt_link_unroute(mqtt_scope_t scope, const char *subtopic);

/* Starts the client (clean session, keep-alive 15 s, auto-reconnect). On every
 * connect it subscribes the routes (and mower/status/server, its own), and
 * publishes retained "online" on gateway/status (last will "offline"). The
 * receive buffer is 8192 bytes so a 4096-byte firmware chunk (plus its 64-byte
 * header) arrives in one MQTT_EVENT_DATA. */
esp_err_t mqtt_link_start(void);

/* Publishes a firmware request on gateway/fw/req (QoS 1, not retained). */
esp_err_t mqtt_link_publish_fw_req(const char *json);

/* Waits until connected and every MQTT_ROUTE_READY route is subscribed. */
bool mqtt_link_wait_ready(int timeout_ms);

/* Publishes on the scope's topic. len 0 means strlen(payload). Counted in the
 * per-topic usage counters. */
esp_err_t mqtt_link_publish(mqtt_scope_t scope, const char *subtopic, const char *payload, int len, int qos,
                            bool retain);

/* Publishes a reply on gateway/cmd/ack (QoS 1). */
esp_err_t mqtt_link_send_ack(const char *json);

bool mqtt_link_connected(void);

/* The server link of §4.2: MQTT is up and mower/status/server says "online". */
bool mqtt_link_server_up(void);

void mqtt_link_stop(void);
