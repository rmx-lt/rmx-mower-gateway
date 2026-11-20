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
#define ROUTES_READY BIT1   /* every MQTT_ROUTE_READY route has its SUBACK */

#define SERVER_STATUS_TOPIC "mower/status/server"

/* reassemble fragmented messages up to this size */
#define MAX_MESSAGE (128 * 1024)

static esp_mqtt_client_handle_t s_client;
static EventGroupHandle_t s_ev;
static char s_prefix[2][64];   /* by scope: mower/<name>/gateway/, mower/<name>/autopilot/ */
static char s_status_topic[96];   /* retained online / offline, also the last will */
static int64_t s_connect_started_us;

/* Subscriptions (wiki Mower-Gateway-Architecture §5.2): subscribed on every
 * connect, and at once when added while connected. Guarded by s_mux; callbacks
 * run outside it. */
#define MAX_ROUTES 24
#define SUB_MAX    48
typedef struct {
    bool used;
    mqtt_scope_t scope;
    char sub[SUB_MAX];
    char topic[112];
    int qos;
    unsigned flags;
    mqtt_route_cb_t cb;
    void *ctx;
    int sub_id;
    bool acked;
} route_t;
static route_t s_routes[MAX_ROUTES];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;   /* the prefixes are known */

/* reassembly of fragmented messages */
static char *s_rx;
static size_t s_rx_len;
static char s_rx_topic[96];

static bool connected(void)
{
    return s_ev && (xEventGroupGetBits(s_ev) & CONNECTED);
}

/* Caller holds s_mux. */
static void build_topic(route_t *r)
{
    if (r->scope == MQTT_SCOPE_ABSOLUTE) {
        snprintf(r->topic, sizeof(r->topic), "%s", r->sub);
    } else {
        snprintf(r->topic, sizeof(r->topic), "%s%s", s_prefix[r->scope], r->sub);
    }
}

/* Sets ROUTES_READY when connected and every READY route is acknowledged. */
static void check_ready(void)
{
    bool ready = true;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (s_routes[i].used && (s_routes[i].flags & MQTT_ROUTE_READY) && !s_routes[i].acked) {
            ready = false;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (ready && connected()) {
        xEventGroupSetBits(s_ev, ROUTES_READY);
    }
}

/* Subscribes route i (outside s_mux: the client call can block). */
static void subscribe_route(int i)
{
    char topic[112];
    portENTER_CRITICAL(&s_mux);
    bool used = s_routes[i].used;
    memcpy(topic, s_routes[i].topic, sizeof(topic));
    int qos = s_routes[i].qos;
    s_routes[i].acked = false;
    portEXIT_CRITICAL(&s_mux);
    if (!used) {
        return;
    }
    int id = esp_mqtt_client_subscribe_single(s_client, topic, qos);
    portENTER_CRITICAL(&s_mux);
    s_routes[i].sub_id = id;
    portEXIT_CRITICAL(&s_mux);
}

static void on_subscribed(int msg_id)
{
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (s_routes[i].used && s_routes[i].sub_id == msg_id) {
            s_routes[i].acked = true;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    check_ready();
}

static void on_server_status(const char *subtopic, const uint8_t *data, size_t len, void *ctx)
{
    bool online = len == 6 && memcmp(data, "online", 6) == 0;
    ESP_LOGI(TAG, "server %s", online ? "online" : "offline");
    gw_link_set(GW_SERVER_ONLINE, online);
}

static void handle_message(const char *topic, const char *data, size_t len)
{
    mqtt_route_cb_t cb = NULL;
    void *ctx = NULL;
    char sub[SUB_MAX];
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (s_routes[i].used && strcmp(topic, s_routes[i].topic) == 0) {
            cb = s_routes[i].cb;
            ctx = s_routes[i].ctx;
            memcpy(sub, s_routes[i].sub, sizeof(sub));
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (cb) {
        cb(sub, (const uint8_t *)data, len, ctx);
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
        for (int i = 0; i < MAX_ROUTES; i++) {
            subscribe_route(i);
        }
        check_ready();   /* no READY routes: ready at once */
        mqtt_link_publish(MQTT_SCOPE_GATEWAY, "status", "online", 0, 1, true);
        clear_legacy_topics();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        xEventGroupClearBits(s_ev, CONNECTED | ROUTES_READY);
        gw_link_set(GW_MQTT_UP, false);
        gw_link_set(GW_SERVER_ONLINE, false);   /* learnt again from the retained message */
        break;
    case MQTT_EVENT_SUBSCRIBED:
        on_subscribed(e->msg_id);
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

esp_err_t mqtt_link_route(mqtt_scope_t scope, const char *subtopic, int qos, unsigned flags,
                          mqtt_route_cb_t cb, void *ctx)
{
    if (!subtopic || !cb || strlen(subtopic) >= SUB_MAX || scope > MQTT_SCOPE_ABSOLUTE ||
            strpbrk(subtopic, "+#")) {
        return ESP_ERR_INVALID_ARG;
    }
    int idx = -1;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (s_routes[i].used && s_routes[i].scope == scope && strcmp(s_routes[i].sub, subtopic) == 0) {
            idx = i;   /* replace the handler of an existing route */
            break;
        }
        if (!s_routes[i].used && idx < 0) {
            idx = i;
        }
    }
    bool fresh = false;
    if (idx >= 0) {
        route_t *r = &s_routes[idx];
        fresh = !r->used || r->qos != qos;
        r->used = true;
        r->scope = scope;
        snprintf(r->sub, sizeof(r->sub), "%s", subtopic);
        r->qos = qos;
        r->flags = flags;
        r->cb = cb;
        r->ctx = ctx;
        if (fresh) {
            r->acked = false;
            r->sub_id = -1;
        }
        if (s_started) {
            build_topic(r);
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (idx < 0) {
        ESP_LOGE(TAG, "route table full, %s not subscribed", subtopic);
        return ESP_ERR_NO_MEM;
    }
    if (fresh && connected()) {
        if (flags & MQTT_ROUTE_READY) {
            xEventGroupClearBits(s_ev, ROUTES_READY);
        }
        subscribe_route(idx);
    }
    return ESP_OK;
}

esp_err_t mqtt_link_unroute(mqtt_scope_t scope, const char *subtopic)
{
    char topic[112] = "";
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (s_routes[i].used && s_routes[i].scope == scope && strcmp(s_routes[i].sub, subtopic) == 0) {
            memcpy(topic, s_routes[i].topic, sizeof(topic));
            s_routes[i].used = false;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (!topic[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    if (connected()) {
        esp_mqtt_client_unsubscribe(s_client, topic);
    }
    check_ready();
    return ESP_OK;
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
    snprintf(s_prefix[MQTT_SCOPE_GATEWAY], sizeof(s_prefix[0]), "mower/%s/gateway/", name);
    snprintf(s_prefix[MQTT_SCOPE_AUTOPILOT], sizeof(s_prefix[0]), "mower/%s/autopilot/", name);
    snprintf(s_status_topic, sizeof(s_status_topic), "%sstatus", s_prefix[MQTT_SCOPE_GATEWAY]);
    mqtt_link_route(MQTT_SCOPE_ABSOLUTE, SERVER_STATUS_TOPIC, 1, 0, on_server_status, NULL);
    portENTER_CRITICAL(&s_mux);
    s_started = true;
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (s_routes[i].used) {
            build_topic(&s_routes[i]);
        }
    }
    portEXIT_CRITICAL(&s_mux);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_GATEWAY_MQTT_CERT_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = name,
        .credentials.client_id = name,   /* the broker forces it anyway */
        .credentials.authentication.certificate = chain,   /* PEM, NUL-terminated */
        .credentials.authentication.key = key,
        .session.keepalive = 15,                  /* wiki Mower-Protocol §3 */
        /* Clean session: the broker queues nothing for us. Our own QoS 1 publishes (acks)
         * wait in esp-mqtt's outbox across a reconnect for up to
         * CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS (sdkconfig.defaults, #88). */
        .session.disable_clean_session = false,
        .session.message_retransmit_timeout = 10000,   /* LTE round trips + TLS */
        .session.last_will = {
            .topic = s_status_topic,
            .msg = "offline",
            .qos = 1,
            .retain = 1,
        },
        .outbox.limit = 32768,   /* a long outage cannot fill the RAM; a full outbox fails the publish */
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
    EventBits_t want = CONNECTED | ROUTES_READY;
    return (xEventGroupWaitBits(s_ev, want, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & want) == want;
}

esp_err_t mqtt_link_publish(mqtt_scope_t scope, const char *subtopic, const char *payload, int len, int qos,
                            bool retain)
{
    if (!s_client || scope > MQTT_SCOPE_ABSOLUTE) {
        return ESP_ERR_INVALID_STATE;
    }
    char topic[112];
    int tl = snprintf(topic, sizeof(topic), "%s%s", scope == MQTT_SCOPE_ABSOLUTE ? "" : s_prefix[scope], subtopic);
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
    return mqtt_link_publish(MQTT_SCOPE_GATEWAY, "fw/req", json, 0, 1, false);
}

esp_err_t mqtt_link_send_ack(const char *json)
{
    return mqtt_link_publish(MQTT_SCOPE_GATEWAY, "cmd/ack", json, 0, 1, false);
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
