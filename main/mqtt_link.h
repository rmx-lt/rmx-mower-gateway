/* MQTT over TLS to Mosquitto: request/response with rmx-server-app. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "probe_proto.h"

/* Called on the MQTT task for each message on mower/<user>/cmd. Must not
 * block: hand the work to another task. */
typedef void (*mqtt_link_cmd_cb_t)(const char *data, size_t len);

/* Set before mqtt_link_start(). */
void mqtt_link_set_cmd_handler(mqtt_link_cmd_cb_t cb);

/* Starts the client (auto-reconnect) and subscribes mower/<user>/res,
 * mower/<user>/fw/chunk, mower/<user>/fw/res, mower/events/records,
 * mower/status/server and (with a cmd handler) mower/<user>/cmd on every
 * connect. The receive buffer is 8192 bytes so a default 4096-byte firmware
 * chunk (plus its 64-byte header) arrives in one MQTT_EVENT_DATA. */
esp_err_t mqtt_link_start(void);

/* Firmware messages, after reassembly. chunk is true for fw/chunk (binary)
 * and false for fw/res (JSON). Called on the MQTT task: must not block.
 * NULL stops delivery. */
typedef void (*mqtt_link_fw_cb_t)(const uint8_t *data, size_t len, bool chunk);

void mqtt_link_set_fw_handler(mqtt_link_fw_cb_t cb);

/* Publishes a firmware request on mower/<user>/fw/req (QoS 1, not retained). */
esp_err_t mqtt_link_publish_fw_req(const char *json);

/* Waits until connected and mower/<user>/res is subscribed. */
bool mqtt_link_wait_ready(int timeout_ms);

/* Publishes a request on mower/<user>/req (QoS 1) and waits for the matching
 * response on mower/<user>/res. One request at a time. */
esp_err_t mqtt_link_request(probe_op_t op, int64_t id, const char *value,
                            probe_result_t *out, int timeout_ms);

/* Publishes on mower/<user>/<subtopic> (QoS 1), e.g. "firmware". */
esp_err_t mqtt_link_publish_own(const char *subtopic, const char *payload, bool retain);

/* Publishes a reply on mower/<user>/cmd/ack (QoS 1). */
esp_err_t mqtt_link_send_ack(const char *json);

void mqtt_link_stop(void);
