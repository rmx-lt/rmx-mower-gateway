#include "mqtt_link.h"
#include "creds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt";

#define CONNECTED  BIT0
#define RES_SUBBED BIT1
#define GOT_RES    BIT2

/* list responses can hold up to 500 records: reassemble up to this size */
#define MAX_MESSAGE (128 * 1024)

static esp_mqtt_client_handle_t s_client;
static EventGroupHandle_t s_ev;
static char s_password[CREDS_MAX_LEN];   /* esp-mqtt keeps the pointer */
static SemaphoreHandle_t s_req_lock;
static char s_req_topic[96];
static char s_res_topic[96];
static char s_cmd_topic[96];
static char s_ack_topic[96];
static char s_status_topic[96];   /* retained online / offline, also the last will */
static mqtt_link_cmd_cb_t s_cmd_cb;
static int s_res_sub_msg_id = -1;
static int64_t s_connect_started_us;

/* the request we wait for, and its parsed answer */
static char s_pending_id[PROBE_REQUEST_ID_LEN + 1];
static probe_result_t s_pending_result;

/* reassembly of fragmented messages */
static char *s_rx;
static size_t s_rx_len;
static char s_rx_topic[96];

static void handle_message(const char *topic, const char *data, size_t len)
{
    if (s_cmd_cb && strcmp(topic, s_cmd_topic) == 0) {
        s_cmd_cb(data, len);
    } else if (strcmp(topic, s_res_topic) == 0) {
        probe_result_t r;
        if (!probe_proto_parse_response(data, len, &r)) {
            ESP_LOGW(TAG, "unparsable response (%u bytes)", (unsigned)len);
            return;
        }
        if (s_pending_id[0] && strcmp(r.request_id, s_pending_id) == 0) {
            s_pending_result = r;
            xEventGroupSetBits(s_ev, GOT_RES);
        } else {
            ESP_LOGW(TAG, "response for unknown request_id %s", r.request_id);
        }
    } else if (strncmp(topic, "probe/events/", 13) == 0) {
        ESP_LOGI(TAG, "event on %s: %.*s", topic, len > 200 ? 200 : (int)len, data);
    } else {
        ESP_LOGI(TAG, "%s: %.*s", topic, len > 200 ? 200 : (int)len, data);
    }
}

static void on_data(esp_mqtt_event_handle_t e)
{
    if (e->current_data_offset == 0) {
        snprintf(s_rx_topic, sizeof(s_rx_topic), "%.*s", e->topic_len, e->topic);
        if (e->total_data_len == e->data_len) {
            handle_message(s_rx_topic, e->data, e->data_len);   /* not fragmented */
            return;
        }
        free(s_rx);
        s_rx = NULL;
        s_rx_len = 0;
        if (e->total_data_len > MAX_MESSAGE) {
            ESP_LOGW(TAG, "message on %s too large (%d bytes), dropped", s_rx_topic, e->total_data_len);
            return;
        }
        s_rx = malloc(e->total_data_len);   /* > 16 KB goes to PSRAM */
        if (!s_rx) {
            ESP_LOGE(TAG, "no memory for %d bytes", e->total_data_len);
            return;
        }
    }
    if (!s_rx) {
        return;
    }
    memcpy(s_rx + e->current_data_offset, e->data, e->data_len);
    s_rx_len = e->current_data_offset + e->data_len;
    if (s_rx_len == (size_t)e->total_data_len) {
        handle_message(s_rx_topic, s_rx, s_rx_len);
        free(s_rx);
        s_rx = NULL;
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_BEFORE_CONNECT:
        s_connect_started_us = esp_timer_get_time();
        break;
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected as %s in %lld ms (session present %d)", creds_device_name(),
                 (esp_timer_get_time() - s_connect_started_us) / 1000, e->session_present);
        xEventGroupSetBits(s_ev, CONNECTED);
        s_res_sub_msg_id = esp_mqtt_client_subscribe_single(s_client, s_res_topic, 1);
        esp_mqtt_client_subscribe_single(s_client, "probe/events/records", 0);
        esp_mqtt_client_subscribe_single(s_client, "probe/status/server", 1);
        if (s_cmd_cb) {
            esp_mqtt_client_subscribe_single(s_client, s_cmd_topic, 1);
        }
        esp_mqtt_client_publish(s_client, s_status_topic, "online", 0, 1, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        xEventGroupClearBits(s_ev, CONNECTED | RES_SUBBED);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        if (e->msg_id == s_res_sub_msg_id) {
            ESP_LOGI(TAG, "subscribed %s", s_res_topic);
            xEventGroupSetBits(s_ev, RES_SUBBED);
        }
        break;
    case MQTT_EVENT_DATA:
        on_data(e);
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "broker refused the login (connack code %d)",
                     e->error_handle->connect_return_code);
        } else {
            ESP_LOGE(TAG, "error type %d, tls esp_err 0x%x, mbedtls -0x%x, sock errno %d",
                     e->error_handle->error_type, e->error_handle->esp_tls_last_esp_err,
                     -e->error_handle->esp_tls_stack_err, e->error_handle->esp_transport_sock_errno);
        }
        break;
    default:
        break;
    }
}

void mqtt_link_set_cmd_handler(mqtt_link_cmd_cb_t cb)
{
    s_cmd_cb = cb;
}

esp_err_t mqtt_link_start(void)
{
    esp_err_t err = creds_get(CREDS_MQTT_PW, s_password, sizeof(s_password));
    if (err != ESP_OK) {
        return err;
    }
    s_ev = xEventGroupCreate();
    s_req_lock = xSemaphoreCreateMutex();
    const char *name = creds_device_name();
    snprintf(s_req_topic, sizeof(s_req_topic), "probe/req/%s", name);
    snprintf(s_res_topic, sizeof(s_res_topic), "probe/res/%s", name);
    snprintf(s_cmd_topic, sizeof(s_cmd_topic), "mower/%s/cmd", name);
    snprintf(s_ack_topic, sizeof(s_ack_topic), "mower/%s/cmd/ack", name);
    snprintf(s_status_topic, sizeof(s_status_topic), "mower/%s/status", name);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_PROBE_MQTT_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = name,
        .credentials.client_id = name,   /* the broker forces it anyway */
        .credentials.authentication.password = s_password,
        .session.keepalive = 60,                  /* below typical CGNAT idle timeouts */
        .session.message_retransmit_timeout = 10000,   /* LTE round trips + TLS */
        .session.last_will = {
            .topic = s_status_topic,
            .msg = "offline",
            .qos = 1,
            .retain = 1,
        },
        .network.reconnect_timeout_ms = 10000,
        .network.timeout_ms = 20000,
        .buffer.size = 4096,
        .buffer.out_size = 2048,
        .task.stack_size = 8192,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    return esp_mqtt_client_start(s_client);
}

bool mqtt_link_wait_ready(int timeout_ms)
{
    if (!s_ev) {
        return false;   /* never started */
    }
    EventBits_t want = CONNECTED | RES_SUBBED;
    return (xEventGroupWaitBits(s_ev, want, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & want) == want;
}

esp_err_t mqtt_link_request(probe_op_t op, int64_t id, const char *value,
                            probe_result_t *out, int timeout_ms)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    char rid[PROBE_REQUEST_ID_LEN + 1];
    probe_proto_new_request_id(rid);
    char *json = probe_proto_build_request(rid, op, id, value);
    esp_err_t err = ESP_OK;

    snprintf(s_pending_id, sizeof(s_pending_id), "%s", rid);
    xEventGroupClearBits(s_ev, GOT_RES);
    int64_t t0 = esp_timer_get_time();
    int msg_id = esp_mqtt_client_publish(s_client, s_req_topic, json, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "publish on %s failed", s_req_topic);
        err = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "-> %s %s", s_req_topic, json);
        if (xEventGroupWaitBits(s_ev, GOT_RES, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms)) & GOT_RES) {
            *out = s_pending_result;
            ESP_LOGI(TAG, "<- %s request_id %s %s in %lld ms", s_res_topic, out->request_id,
                     out->ok ? "ok" : "error", (esp_timer_get_time() - t0) / 1000);
        } else {
            ESP_LOGE(TAG, "no response to %s within %d ms", rid, timeout_ms);
            err = ESP_ERR_TIMEOUT;
        }
    }
    s_pending_id[0] = '\0';
    free(json);
    xSemaphoreGive(s_req_lock);
    return err;
}

esp_err_t mqtt_link_publish_own(const char *subtopic, const char *payload, bool retain)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    char topic[96];
    snprintf(topic, sizeof(topic), "mower/%s/%s", creds_device_name(), subtopic);
    if (esp_mqtt_client_publish(s_client, topic, payload, 0, 1, retain) < 0) {
        ESP_LOGE(TAG, "publish on %s failed", topic);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "-> %s %s", topic, payload);
    return ESP_OK;
}

esp_err_t mqtt_link_send_ack(const char *json)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, s_ack_topic, json, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "publish on %s failed", s_ack_topic);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "-> %s %s", s_ack_topic, json);
    return ESP_OK;
}

void mqtt_link_stop(void)
{
    if (!s_client) {
        return;
    }
    esp_mqtt_client_publish(s_client, s_status_topic, "offline", 0, 1, 1);
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
}
