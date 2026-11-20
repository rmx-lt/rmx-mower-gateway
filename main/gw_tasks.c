#include "gw_tasks.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "recovery.h"

static const char *TAG = "tasks";

typedef struct {
    const char *name;
    UBaseType_t prio;
    uint32_t stack;
} gw_task_def_t;

/* Priorities are relative; stacks are resized from the high-water marks in the alive log. */
static const gw_task_def_t s_tasks[GW_T_COUNT] = {
    [GW_T_REMOTE_CMD] = { "remote_cmd", 4, 6144 },
    [GW_T_TICK]       = { "gw_tick",    3, 6144 },
    [GW_T_SIGNAL]     = { "gw_signal",  2, 4096 },
    [GW_T_CELL]       = { "cell_wd",    5, 4096 },
    [GW_T_SUPERVISOR] = { "supervisor", 3, 6144 },
    [GW_T_OTA]        = { "ota",        3, 12288 },
    [GW_T_DEVCERT]    = { "devcert",    3, 6144 },
};

/* ESP-IDF tasks whose stacks come from sdkconfig, logged for sizing too. */
static const char *const s_idf_tasks[] = { "mqtt_task", "tcpip", "sys_evt" };

static volatile uint32_t s_alive[GW_T_WATCHED];   /* each written only by its own task */
static live_t s_live[GW_T_WATCHED];               /* supervisor only */

BaseType_t gw_task_create(gw_task_id_t id, TaskFunction_t fn, void *arg, TaskHandle_t *handle)
{
    const gw_task_def_t *t = &s_tasks[id];
    return xTaskCreate(fn, t->name, t->stack, arg, t->prio, handle);
}

void gw_task_alive(gw_task_id_t id)
{
    if (id < GW_T_WATCHED) {
        s_alive[id]++;
    }
}

const char *gw_tasks_stale(int64_t now_ms, int64_t stale_ms, bool ota_writing)
{
    uint32_t counters[GW_T_WATCHED];
    for (int i = 0; i < GW_T_WATCHED; i++) {
        counters[i] = s_alive[i];
    }
    int i = live_check(s_live, counters, GW_T_WATCHED, now_ms, stale_ms, ota_writing);
    return i >= 0 ? s_tasks[i].name : NULL;
}

static void add_stack(char *buf, size_t size, const char *name)
{
    TaskHandle_t h = xTaskGetHandle(name);
    if (h) {
        size_t n = strlen(buf);
        snprintf(buf + n, size - n, " %s %u", name, (unsigned)uxTaskGetStackHighWaterMark(h));
    }
}

void gw_tasks_log_stacks(void)
{
    char buf[256] = "";
    for (int i = 0; i < GW_T_COUNT; i++) {
        add_stack(buf, sizeof(buf), s_tasks[i].name);
    }
    for (int i = 0; i < sizeof(s_idf_tasks) / sizeof(s_idf_tasks[0]); i++) {
        add_stack(buf, sizeof(buf), s_idf_tasks[i]);
    }
    ESP_LOGI(TAG, "stack free (bytes):%s", buf);
}
