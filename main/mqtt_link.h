/* MQTT over TLS to Mosquitto: the gateway's topics of wiki Mower-Protocol §3
 * (mower/<name>/gateway/...), commands, firmware chunks and the server link. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Called on the MQTT task for each message on gateway/cmd. Must not block:
 * hand the work to another task. */
typedef void (*mqtt_link_cmd_cb_t)(const char *data, size_t len);

/* Set before mqtt_link_start(). */
void mqtt_link_set_cmd_handler(mqtt_link_cmd_cb_t cb);

/* Starts the client (clean session, keep-alive 15 s, auto-reconnect). On every
 * connect it subscribes gateway/cmd, gateway/fw/chunk, gateway/fw/res and
 * mower/status/server, and publishes retained "online" on gateway/status (last
 * will "offline"). The receive buffer is 8192 bytes so a 4096-byte firmware
 * chunk (plus its 64-byte header) arrives in one MQTT_EVENT_DATA. */
esp_err_t mqtt_link_start(void);

/* Firmware messages, after reassembly. chunk is true for gateway/fw/chunk
 * (binary) and false for gateway/fw/res (JSON). Called on the MQTT task: must
 * not block. NULL stops delivery. */
typedef void (*mqtt_link_fw_cb_t)(const uint8_t *data, size_t len, bool chunk);

void mqtt_link_set_fw_handler(mqtt_link_fw_cb_t cb);

/* Publishes a firmware request on gateway/fw/req (QoS 1, not retained). */
esp_err_t mqtt_link_publish_fw_req(const char *json);

/* Waits until connected and the firmware topics are subscribed. */
bool mqtt_link_wait_ready(int timeout_ms);

/* Publishes on mower/<name>/gateway/<subtopic>. len 0 means strlen(payload).
 * Counted in the per-topic usage counters. */
esp_err_t mqtt_link_publish(const char *subtopic, const char *payload, int len, int qos, bool retain);

/* Publishes a reply on gateway/cmd/ack (QoS 1). */
esp_err_t mqtt_link_send_ack(const char *json);

bool mqtt_link_connected(void);

/* The server link of §4.2: MQTT is up and mower/status/server says "online". */
bool mqtt_link_server_up(void);

void mqtt_link_stop(void);
