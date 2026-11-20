#include "mqtt_link.h"
#include "creds.h"
#include "devcert.h"
#include "gw_link.h"
#include "gwstate.h"
#include "usage.h"

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

#define SERVER_STATUS_TOPIC "mower/status/server"

/* reassemble fragmented messages up to this size */
#define MAX_MESSAGE (128 * 1024)

static esp_mqtt_client_handle_t s_client;
static EventGroupHandle_t s_ev;
static char s_prefix[64];   /* mower/<name>/gateway/ */
static char s_cmd_topic[96];
static char s_status_topic[96];   /* retained online / offline, also the last will */
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
    if (strcmp(topic, SERVER_STATUS_TOPIC) == 0) {
        bool online = len == 6 && memcmp(data, "online", 6) == 0;
        ESP_LOGI(TAG, "server %s", online ? "online" : "offline");
        gw_link_set(GW_SERVER_ONLINE, online);
        return;
    }
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
    } else {
        ESP_LOGI(TAG, "%s: %u bytes, no handler", topic, (unsigned)len);   /* never the payload */
    }
}

static void on_data(esp_mqtt_event_handle_t e)
{
    if (e->current_data_offset == 0) {
        usage_mqtt_count(false, e->topic, e->topic_len, e->total_data_len, e->qos);
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

/* The unprefixed topics of the firmware before the protocol move leave retained
 * messages on the broker ("online" would stay for good). Clear them once. */
static void clear_legacy_topics(void)
{
    char flag[4];
    if (gw_nvs_get("legacy", flag, sizeof(flag))) {
        return;
    }
    const char *old[] = { "status", "firmware" };
    for (size_t i = 0; i < sizeof(old) / sizeof(old[0]); i++) {
        char topic[96];
        snprintf(topic, sizeof(topic), "mower/%s/%s", creds_device_name(), old[i]);
        if (esp_mqtt_client_publish(s_client, topic, "", 0, 1, 1) < 0) {
            return;
        }
    }
    gw_nvs_set("legacy", "1");
    ESP_LOGI(TAG, "cleared the retained status and firmware messages of the old topics");
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
        gw_link_set(GW_MQTT_UP, true);
        esp_mqtt_client_subscribe_single(s_client, SERVER_STATUS_TOPIC, 1);
        if (s_cmd_cb) {
            esp_mqtt_client_subscribe_single(s_client, s_cmd_topic, 1);
        }
        s_fw_chunk_sub_id = esp_mqtt_client_subscribe_single(s_client, s_fw_chunk_topic, 1);
        s_fw_res_sub_id = esp_mqtt_client_subscribe_single(s_client, s_fw_res_topic, 1);
        mqtt_link_publish("status", "online", 0, 1, true);
        clear_legacy_topics();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        xEventGroupClearBits(s_ev, CONNECTED | FW_CHUNK_SUB | FW_RES_SUB);
        gw_link_set(GW_MQTT_UP, false);
        gw_link_set(GW_SERVER_ONLINE, false);   /* learnt again from the retained message */
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
                     e->error_handle->esp_tls_stack_err,   /* esp-tls stores -ret: already positive */
                     e->error_handle->esp_transport_sock_errno);
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
    char own[48];
    snprintf(own, sizeof(own), "mower/%s/", name);
    usage_set_prefix(own);
    snprintf(s_prefix, sizeof(s_prefix), "mower/%s/gateway/", name);
    snprintf(s_cmd_topic, sizeof(s_cmd_topic), "%scmd", s_prefix);
    snprintf(s_status_topic, sizeof(s_status_topic), "%sstatus", s_prefix);
    snprintf(s_fw_chunk_topic, sizeof(s_fw_chunk_topic), "%sfw/chunk", s_prefix);
    snprintf(s_fw_res_topic, sizeof(s_fw_res_topic), "%sfw/res", s_prefix);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_GATEWAY_MQTT_CERT_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = name,
        .credentials.client_id = name,   /* the broker forces it anyway */
        .credentials.authentication.certificate = chain,   /* PEM, NUL-terminated */
        .credentials.authentication.key = key,
        .session.keepalive = 15,                  /* wiki Mower-Protocol §3 */
        .session.disable_clean_session = false,   /* clean session: nothing is queued while offline */
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
         * MQTT framing (wiki Mower-Protocol §12). Larger frames are still
         * reassembled in on_data. out_size holds a 4096-byte JSON message (a
         * catalogue part) plus framing. */
        .buffer.size = 8192,
        .buffer.out_size = 4608,
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

esp_err_t mqtt_link_publish(const char *subtopic, const char *payload, int len, int qos, bool retain)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    char topic[96];
    int tl = snprintf(topic, sizeof(topic), "%s%s", s_prefix, subtopic);
    if (len == 0) {
        len = (int)strlen(payload);
    }
    if (esp_mqtt_client_publish(s_client, topic, payload, len, qos, retain) < 0) {
        ESP_LOGE(TAG, "publish on %s failed", topic);
        return ESP_FAIL;
    }
    usage_mqtt_count(true, topic, tl, len, qos);
    if (qos > 0) {
        ESP_LOGI(TAG, "-> %s %.*s%s", topic, len > 300 ? 300 : len, payload, len > 300 ? "..." : "");
    }
    return ESP_OK;
}

esp_err_t mqtt_link_publish_fw_req(const char *json)
{
    return mqtt_link_publish("fw/req", json, 0, 1, false);
}

esp_err_t mqtt_link_send_ack(const char *json)
{
    return mqtt_link_publish("cmd/ack", json, 0, 1, false);
}

bool mqtt_link_connected(void)
{
    return gw_link_get() & GW_MQTT_UP;
}

bool mqtt_link_server_up(void)
{
    return gw_link_get() & GW_SERVER_LINK_UP;
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
