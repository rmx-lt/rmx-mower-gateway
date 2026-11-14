#include "mqtt_link.h"
#include "creds.h"
#include "devcert.h"

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

#define CONNECTED    BIT0
#define FW_CHUNK_SUB BIT3
#define FW_RES_SUB   BIT4

/* reassemble fragmented messages up to this size */
#define MAX_MESSAGE (128 * 1024)

static esp_mqtt_client_handle_t s_client;
static EventGroupHandle_t s_ev;
static char s_cmd_topic[96];
static char s_ack_topic[96];
static char s_status_topic[96];   /* retained online / offline, also the last will */
static char s_fw_req_topic[96];
static char s_fw_chunk_topic[96];
static char s_fw_res_topic[96];
static mqtt_link_cmd_cb_t s_cmd_cb;
static mqtt_link_fw_cb_t s_fw_cb;
static int s_fw_chunk_sub_id = -1;
static int s_fw_res_sub_id = -1;
static int64_t s_connect_started_us;

/* reassembly of fragmented messages */
static char *s_rx;
static size_t s_rx_len;
static char s_rx_topic[96];

static void handle_message(const char *topic, const char *data, size_t len)
{
    if (strcmp(topic, s_fw_chunk_topic) == 0) {
        if (s_fw_cb) {
            s_fw_cb((const uint8_t *)data, len, true);
        }
        return;
    }
    if (strcmp(topic, s_fw_res_topic) == 0) {
        if (s_fw_cb) {
            s_fw_cb((const uint8_t *)data, len, false);
        } else {
            ESP_LOGI(TAG, "fw res: %.*s", len > 200 ? 200 : (int)len, data);
        }
        return;
    }
    if (s_cmd_cb && strcmp(topic, s_cmd_topic) == 0) {
        s_cmd_cb(data, len);
    } else if (strncmp(topic, "mower/events/", 13) == 0) {
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
        esp_mqtt_client_subscribe_single(s_client, "mower/status/server", 1);
        if (s_cmd_cb) {
            esp_mqtt_client_subscribe_single(s_client, s_cmd_topic, 1);
        }
        s_fw_chunk_sub_id = esp_mqtt_client_subscribe_single(s_client, s_fw_chunk_topic, 1);
        s_fw_res_sub_id = esp_mqtt_client_subscribe_single(s_client, s_fw_res_topic, 1);
        esp_mqtt_client_publish(s_client, s_status_topic, "online", 0, 1, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        xEventGroupClearBits(s_ev, CONNECTED | FW_CHUNK_SUB | FW_RES_SUB);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        if (e->msg_id == s_fw_chunk_sub_id) {
            xEventGroupSetBits(s_ev, FW_CHUNK_SUB);
        } else if (e->msg_id == s_fw_res_sub_id) {
            xEventGroupSetBits(s_ev, FW_RES_SUB);
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

void mqtt_link_set_fw_handler(mqtt_link_fw_cb_t cb)
{
    s_fw_cb = cb;
}

esp_err_t mqtt_link_start(void)
{
    /* The only login is the client certificate (its CN is the name, no password). */
    const char *chain = devcert_chain_pem();
    const char *key = devcert_key_pem();
    if (!chain || !key) {
        ESP_LOGE(TAG, "no client certificate: not connecting (tools/provision_cert.ps1)");
        return ESP_ERR_INVALID_STATE;
    }
    s_ev = xEventGroupCreate();
    const char *name = creds_device_name();
    snprintf(s_cmd_topic, sizeof(s_cmd_topic), "mower/%s/cmd", name);
    snprintf(s_ack_topic, sizeof(s_ack_topic), "mower/%s/cmd/ack", name);
    snprintf(s_status_topic, sizeof(s_status_topic), "mower/%s/status", name);
    snprintf(s_fw_req_topic, sizeof(s_fw_req_topic), "mower/%s/fw/req", name);
    snprintf(s_fw_chunk_topic, sizeof(s_fw_chunk_topic), "mower/%s/fw/chunk", name);
    snprintf(s_fw_res_topic, sizeof(s_fw_res_topic), "mower/%s/fw/res", name);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_GATEWAY_MQTT_CERT_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = name,
        .credentials.client_id = name,   /* the broker forces it anyway */
        .credentials.authentication.certificate = chain,   /* PEM, NUL-terminated */
        .credentials.authentication.key = key,
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
        /* 8192 holds a 4096-byte firmware chunk, its 64-byte header and the
         * MQTT framing (wiki Server-App, "Firmware over MQTT"). Larger frames
         * are still reassembled in on_data. out_size stays small: publishes
         * from the device are short JSON. */
        .buffer.size = 8192,
        .buffer.out_size = 2048,
        .task.stack_size = 8192,
    };
    ESP_LOGI(TAG, "login: client certificate");
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
    EventBits_t want = CONNECTED | FW_CHUNK_SUB | FW_RES_SUB;
    return (xEventGroupWaitBits(s_ev, want, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & want) == want;
}

esp_err_t mqtt_link_publish_fw_req(const char *json)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_mqtt_client_publish(s_client, s_fw_req_topic, json, 0, 1, 0) < 0) {
        ESP_LOGE(TAG, "publish on %s failed", s_fw_req_topic);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "-> %s %s", s_fw_req_topic, json);
    return ESP_OK;
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
