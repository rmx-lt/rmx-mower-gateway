#include "remote_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "creds.h"
#include "gw_tasks.h"
#include "gwstate.h"
#include "mqtt_link.h"
#include "ota.h"
#include "supervisor.h"

static const char *TAG = "cmd";

#define MAX_CMD  4096   /* wiki Mower-Protocol §4: a JSON message is at most 4096 bytes */
#define MAX_REGS 16

typedef struct {
    char *buf;
    size_t len;
} cmd_msg_t;

typedef struct {
    const char *name;
    cmd_fn_t fn;
    unsigned flags;
} reg_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;   /* guards s_mem */
static cmd_mem_t s_mem;            /* the ids seen in the last minutes with their last ack (§5) */
static reg_t s_regs[MAX_REGS];
static int s_nregs;

esp_err_t cmd_register(const char *name, cmd_fn_t fn, unsigned flags)
{
    if (!cmd_name_valid(name) || !fn) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_nregs; i++) {
        if (strcmp(s_regs[i].name, name) == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (s_nregs == MAX_REGS) {
        ESP_LOGE(TAG, "command table full, %s not registered", name);
        return ESP_ERR_NO_MEM;
    }
    s_regs[s_nregs++] = (reg_t){ name, fn, flags };
    return ESP_OK;
}

static const reg_t *find_reg(const char *name)
{
    for (int i = 0; i < s_nregs; i++) {
        if (strcmp(s_regs[i].name, name) == 0) {
            return &s_regs[i];
        }
    }
    return NULL;
}

/* Runs on the MQTT task: copy and hand over, never block. */
static void on_cmd(const char *subtopic, const uint8_t *data, size_t len, void *ctx)
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

static void mem_store(const char *id, const char *json, bool open)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cmd_mem_store(&s_mem, id, json, open, esp_timer_get_time());
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
        cJSON_AddStringToObject(ack, "error", error ? error : CMD_E_FAILED);
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
    const char *ack = cmd_mem_find(&s_mem, id, esp_timer_get_time());
    if (ack) {
        copy = strdup(ack);
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

static cmd_expires_t expires_kind(const cJSON *item)
{
    return !item ? CMD_EXPIRES_ABSENT : cJSON_IsNumber(item) ? CMD_EXPIRES_NUMBER : CMD_EXPIRES_OTHER;
}

static void unknown(const char *id, const char *name)
{
    char known[160] = "known:";
    for (int i = 0; i < s_nregs; i++) {
        size_t n = strlen(known);
        snprintf(known + n, sizeof(known) - n, " %s", s_regs[i].name);
    }
    cmd_ack(id, name, "failed", CMD_E_UNKNOWN_CMD, known, NULL);
}

static void handle(const char *json, size_t len)
{
    cJSON *cmd = cJSON_ParseWithLength(json, len);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "id"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "cmd"));

    if (!cJSON_IsObject(cmd) || !cmd_id_valid(id) || !cmd_name_valid(name)) {
        ESP_LOGW(TAG, "<- malformed command (%u bytes)", (unsigned)len);
        cmd_ack(cmd_id_valid(id) ? id : "", name, "failed", CMD_E_BAD_REQUEST,
                "need a JSON object with an \"id\" (1-64 chars of A-Za-z0-9_-) and a \"cmd\"", NULL);
        cJSON_Delete(cmd);
        return;
    }
    const reg_t *reg = find_reg(name);
    /* Never log the payload at info level: arguments can be secrets (ntrip_set's password). */
    ESP_LOGI(TAG, "<- %s id %s", name, id);
    if (reg && !(reg->flags & CMD_SECRET_ARGS)) {
        ESP_LOGD(TAG, "<- %.*s", len > 300 ? 300 : (int)len, json);
    }
    if (repeat_ack(id)) {
        cJSON_Delete(cmd);
        return;
    }

    const cJSON *exp = cJSON_GetObjectItem(cmd, "expires_at");
    const char *message = NULL;
    const char *error = cmd_check_expires(expires_kind(exp), cJSON_GetNumberValue(exp), gw_time(), &message);
    cmd_reply_t r = { 0 };
    if (error) {
        cmd_ack(id, name, "failed", error, message, NULL);
    } else if (!reg) {
        unknown(id, name);
    } else if (ota_busy() && !(reg->flags & CMD_DURING_OTA)) {
        cmd_ack(id, name, "failed", CMD_E_BUSY, "a firmware update is running", NULL);
    } else {
        reg->fn(id, cmd, &r);
        if (r.error) {
            cmd_ack(id, name, "failed", r.error, r.message, r.result);
        } else if (!r.accepted) {
            cmd_ack(id, name, "done", NULL, NULL, r.result);
        } else {
            cJSON_Delete(r.result);   /* the handler acks the end itself */
        }
    }
    cJSON_Delete(cmd);

    if (r.restart && !r.error) {
        gw_restart("restart command", 2000);   /* 2 s: let the ack go out */
    }
}

static void cmd_task(void *arg)
{
    cmd_msg_t m;
    for (;;) {
        gw_task_alive(GW_T_REMOTE_CMD);
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(10000)) == pdTRUE) {
            handle(m.buf, m.len);
            free(m.buf);
        }
    }
}

static void cmd_ping(const char *id, const cJSON *cmd, cmd_reply_t *r)
{
    r->result = cJSON_CreateObject();
    cJSON_AddStringToObject(r->result, "pong", creds_device_name());
}

static void cmd_restart(const char *id, const cJSON *cmd, cmd_reply_t *r)
{
    r->restart = true;
}

/* shortcut: the board link (#62) registers the real one; until then the line is not wired. */
static void cmd_autopilot_reset(const char *id, const cJSON *cmd, cmd_reply_t *r)
{
    r->error = CMD_E_FAILED;
    r->message = "the autopilot reset line is not wired";
}

esp_err_t remote_cmd_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(4, sizeof(cmd_msg_t));
    if (!s_lock || !s_queue || gw_task_create(GW_T_REMOTE_CMD, cmd_task, NULL, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    cmd_register("ping", cmd_ping, 0);
    cmd_register("restart", cmd_restart, 0);
    cmd_register("autopilot_reset", cmd_autopilot_reset, 0);
    return mqtt_link_route(MQTT_SCOPE_GATEWAY, "cmd", 1, 0, on_cmd, NULL);
}
