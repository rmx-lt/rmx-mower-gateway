#include "remote_cmd.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cell.h"
#include "creds.h"
#include "gwstate.h"
#include "metrics.h"
#include "mqtt_link.h"
#include "ota.h"

static const char *TAG = "cmd";

#define MAX_CMD     4096   /* wiki Mower-Protocol §4: a JSON message is at most 4096 bytes */
#define ID_LEN      64
#define MEM_IDS     16
#define MEM_US      (180 * 1000000LL)   /* the contract asks for at least 2 minutes */

static QueueHandle_t s_queue;

typedef struct {
    char *buf;
    size_t len;
} cmd_msg_t;

/* The ids seen in the last minutes with the last ack sent for each (§5). */
typedef struct {
    char id[ID_LEN + 1];
    char *ack;          /* last ack, JSON; NULL = slot free */
    bool open;          /* last ack was "accepted": the command still runs */
    int64_t t_us;
} mem_t;

static mem_t s_mem[MEM_IDS];
static SemaphoreHandle_t s_lock;

/* Runs on the MQTT task: copy and hand over, never block. */
static void on_cmd(const char *data, size_t len)
{
    if (len > MAX_CMD) {
        ESP_LOGW(TAG, "command too large (%u bytes), dropped", (unsigned)len);
        return;
    }
    cmd_msg_t m = { .buf = malloc(len + 1), .len = len };
    if (!m.buf) {
        return;
    }
    memcpy(m.buf, data, len);
    m.buf[len] = '\0';
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, command dropped");
        free(m.buf);
    }
}

static bool valid_id(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n < 1 || n > ID_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* Caller holds s_lock. */
static mem_t *mem_find(const char *id)
{
    for (int i = 0; i < MEM_IDS; i++) {
        if (s_mem[i].ack && strcmp(s_mem[i].id, id) == 0) {
            return &s_mem[i];
        }
    }
    return NULL;
}

/* Caller holds s_lock. Takes a free slot, else an expired one, else the oldest
 * finished one, else the oldest. */
static mem_t *mem_slot(void)
{
    int64_t now = esp_timer_get_time();
    mem_t *oldest_done = NULL;
    mem_t *oldest = NULL;
    for (int i = 0; i < MEM_IDS; i++) {
        mem_t *m = &s_mem[i];
        if (!m->ack) {
            return m;
        }
        if (!m->open && now - m->t_us > MEM_US) {
            free(m->ack);
            m->ack = NULL;
            return m;
        }
        if (!m->open && (!oldest_done || m->t_us < oldest_done->t_us)) {
            oldest_done = m;
        }
        if (!oldest || m->t_us < oldest->t_us) {
            oldest = m;
        }
    }
    mem_t *m = oldest_done ? oldest_done : oldest;
    free(m->ack);
    m->ack = NULL;
    return m;
}

static void mem_store(const char *id, const char *json, bool open)
{
    if (!valid_id(id)) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mem_t *m = mem_find(id);
    char *copy = strdup(json);
    if (m && copy) {
        free(m->ack);
    } else if (copy) {
        m = mem_slot();
        snprintf(m->id, sizeof(m->id), "%s", id);
    }
    if (m && copy) {
        m->ack = copy;
        m->open = open;
        m->t_us = esp_timer_get_time();
    } else {
        free(copy);
    }
    xSemaphoreGive(s_lock);
}

static char *build_ack(const char *id, const char *cmd, const char *state, const char *error,
                       const char *message, cJSON *result)
{
    bool ok = strcmp(state, "failed") != 0;
    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "id", id ? id : "");
    cJSON_AddStringToObject(ack, "cmd", cmd ? cmd : "");
    cJSON_AddBoolToObject(ack, "ok", ok);
    cJSON_AddStringToObject(ack, "state", state);
    if (result) {
        cJSON_AddItemToObject(ack, "result", result);
    } else if (strcmp(state, "done") == 0) {
        cJSON_AddObjectToObject(ack, "result");
    }
    if (!ok) {
        cJSON_AddStringToObject(ack, "error", error ? error : "failed");
        cJSON_AddStringToObject(ack, "message", message ? message : "");
    }
    gw_add_time(ack, true);
    char *json = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    return json;
}

void cmd_ack(const char *id, const char *cmd, const char *state, const char *error,
             const char *message, cJSON *result)
{
    char *json = build_ack(id, cmd, state, error, message, result);
    if (!json) {
        return;
    }
    mqtt_link_send_ack(json);
    mem_store(id, json, strcmp(state, "accepted") == 0);
    free(json);
}

void cmd_remember_accepted(const char *id, const char *cmd)
{
    char *json = build_ack(id, cmd, "accepted", NULL, NULL, NULL);
    if (json) {
        mem_store(id, json, true);
        free(json);
    }
}

/* The same id again: send the last ack again, do not run the command. */
static bool repeat_ack(const char *id)
{
    char *copy = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mem_t *m = mem_find(id);
    if (m) {
        m->t_us = esp_timer_get_time();
        copy = strdup(m->ack);
    }
    xSemaphoreGive(s_lock);
    if (!copy) {
        return false;
    }
    ESP_LOGW(TAG, "command id %s seen before, repeating its last ack", id);
    mqtt_link_send_ack(copy);
    free(copy);
    return true;
}

static void restart_soon(void)
{
    ESP_LOGW(TAG, "restart requested, restarting in 2 s");
    vTaskDelay(pdMS_TO_TICKS(2000));   /* let the ack go out */
    mqtt_link_stop();
    cell_stop();
    esp_restart();
}

static void handle(const char *json, size_t len)
{
    ESP_LOGI(TAG, "<- %.*s", len > 300 ? 300 : (int)len, json);
    cJSON *cmd = cJSON_ParseWithLength(json, len);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "id"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "cmd"));
    const cJSON *expires_at = cJSON_GetObjectItem(cmd, "expires_at");
    bool restart = false;

    if (!cJSON_IsObject(cmd) || !valid_id(id) || !name || !name[0] || strlen(name) > 32) {
        cmd_ack(id && valid_id(id) ? id : "", name, "failed", "bad_request",
                "need a JSON object with an \"id\" (1-64 chars of A-Za-z0-9_-) and a \"cmd\"", NULL);
        cJSON_Delete(cmd);
        return;
    }
    if (repeat_ack(id)) {
        cJSON_Delete(cmd);
        return;
    }

    if (expires_at && !cJSON_IsNumber(expires_at)) {
        cmd_ack(id, name, "failed", "bad_request", "expires_at must be a unix time in seconds", NULL);
    } else if (expires_at && !gw_clock_valid()) {
        cmd_ack(id, name, "failed", "no_clock", "the device clock is not set, cannot check expires_at", NULL);
    } else if (expires_at && gw_time() > cJSON_GetNumberValue(expires_at)) {
        cmd_ack(id, name, "failed", "expired", "command expired before it arrived", NULL);
    } else if (ota_busy() && strcmp(name, "monitor_start") != 0 && strcmp(name, "monitor_stop") != 0) {
        cmd_ack(id, name, "failed", "busy", "a firmware update is running", NULL);
    } else if (strcmp(name, "ping") == 0) {
        cJSON *res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "pong", creds_device_name());
        cmd_ack(id, name, "done", NULL, NULL, res);
    } else if (strcmp(name, "restart") == 0) {
        cmd_ack(id, name, "done", NULL, NULL, NULL);
        restart = true;
    } else if (strcmp(name, "ota") == 0) {
        const cJSON *size_item = cJSON_GetObjectItem(cmd, "size");
        const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "version"));
        const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "file"));
        const char *error = "bad_request";
        const char *message = "";
        char file_buf[129];
        if (!file && cJSON_GetObjectItem(cmd, "file")) {
            message = "file must be a string";
        } else if (size_item && (!cJSON_IsNumber(size_item) || cJSON_GetNumberValue(size_item) < 0)) {
            message = "size must be the image size in bytes";
        } else if (!version || !version[0]) {
            message = "ota needs a string version";
        } else {
            if (!file || !file[0]) {
                snprintf(file_buf, sizeof(file_buf), "%s.bin", version);
                file = file_buf;
            }
            /* ota_start sends the "accepted" ack itself, before the task can fail */
            if (ota_start(id, file, version, size_item ? (int)cJSON_GetNumberValue(size_item) : 0,
                          &error, &message)) {
                error = NULL;
            }
        }
        if (error) {
            cmd_ack(id, name, "failed", error, message, NULL);
        }
    } else if (strcmp(name, "monitor_start") == 0) {
        cJSON *res = cJSON_CreateObject();
        const char *error = NULL, *message = NULL;
        if (monitor_start(cmd, res, &error, &message)) {
            cmd_ack(id, name, "done", NULL, NULL, res);
        } else {
            cmd_ack(id, name, "failed", error, message, res);
        }
    } else if (strcmp(name, "monitor_stop") == 0) {
        cJSON *res = cJSON_CreateObject();
        monitor_stop(cmd, res);
        cmd_ack(id, name, "done", NULL, NULL, res);
    } else if (strcmp(name, "autopilot_reset") == 0) {
        cmd_ack(id, name, "failed", "failed", "the autopilot reset line is not wired", NULL);
    } else {
        cmd_ack(id, name, "failed", "unknown_cmd", "known: ping restart ota monitor_start monitor_stop", NULL);
    }
    cJSON_Delete(cmd);

    if (restart) {
        restart_soon();
    }
}

static void cmd_task(void *arg)
{
    cmd_msg_t m;
    for (;;) {
        if (xQueueReceive(s_queue, &m, portMAX_DELAY) == pdTRUE) {
            handle(m.buf, m.len);
            free(m.buf);
        }
    }
}

esp_err_t remote_cmd_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(4, sizeof(cmd_msg_t));
    if (!s_lock || !s_queue || xTaskCreate(cmd_task, "remote_cmd", 6144, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    mqtt_link_set_cmd_handler(on_cmd);
    return ESP_OK;
}
