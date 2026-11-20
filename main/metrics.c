#include "metrics.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cell.h"
#include "gw_link.h"
#include "gwstate.h"
#include "monitor.h"
#include "mqtt_link.h"
#include "gw_tasks.h"
#include "ota.h"
#include "usage.h"

static const char *TAG = "metrics";

/* Bump when the catalogue changes (names, units, periods, rates, ...). */
#define CATALOGUE_REVISION 1

#define TICK_MS     100
#define MAX_METRICS 32

/* ---------------------------------------------------------------- catalogue */

/* Monitor rates (samples per second) the monitorable metrics offer. */
static const int s_rates[] = { 1, 2, 5, 10 };
#define N_RATES (sizeof(s_rates) / sizeof(s_rates[0]))

static const char *const s_parts[] = { "signal", "usage", "system" };
static const char *const s_part_labels[] = { "Signal", "Data usage", "System" };
#define N_PARTS (sizeof(s_parts) / sizeof(s_parts[0]))

#define FAM_UP_PREFIX   "usage.mqtt.up."
#define FAM_DOWN_PREFIX "usage.mqtt.down."

typedef struct {
    const metric_def_t *def;
    metric_read_fn fn;
    void *ctx;
    int8_t family;   /* 0 a metric, 1 usage.mqtt.up.*, 2 usage.mqtt.down.* */
} metric_t;

static metric_t s_metrics[MAX_METRICS];
static int s_n;
static bool s_published;   /* the catalogue went out: the set is fixed */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t add(const metric_def_t *def, metric_read_fn fn, void *ctx, int8_t family)
{
    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&s_mux);
    int i = 0;
    while (i < s_n && strcmp(s_metrics[i].def->name, def->name) != 0) {
        i++;
    }
    if (i < s_n) {
        /* already there */
    } else if (s_published) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s_n == MAX_METRICS) {
        err = ESP_ERR_NO_MEM;
    } else {
        s_metrics[s_n++] = (metric_t){ def, fn, ctx, family };
    }
    portEXIT_CRITICAL(&s_mux);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "metric %s not registered: %s", def->name, esp_err_to_name(err));
    }
    return err;
}

esp_err_t metrics_register(const metric_def_t *def, metric_read_fn fn, void *ctx)
{
    if (!def || !def->name || !def->part || !fn) {
        return ESP_ERR_INVALID_ARG;
    }
    return add(def, fn, ctx, 0);
}

/* ------------------------------------------------------------ system metrics */

typedef enum { SYS_CPU, SYS_CORE0, SYS_CORE1, SYS_MEM_FREE, SYS_MEM_MIN, SYS_MEM_LARGEST, SYS_MEM_PCT, SYS_NVS_PCT,
               SYS_COUNT } sys_id_t;

static const metric_def_t s_sys_defs[SYS_COUNT] = {
    [SYS_CPU] = { .name = "system.cpu", .label = "Processor load",
        .description = "Average load of both processor cores.",
        .unit = "%", .part = "system", .period = 60, .monitor = true, .chart = true,
        .has_min = true, .min = 0, .has_max = true, .max = 100, .has_high = true, .high = 90, .decimals = 1 },
    [SYS_CORE0] = { .name = "system.core0", .label = "Processor load, core 0",
        .description = "Load of processor core 0.",
        .unit = "%", .part = "system", .period = 60, .monitor = true, .chart = true,
        .has_min = true, .min = 0, .has_max = true, .max = 100, .has_high = true, .high = 90, .decimals = 1 },
    [SYS_CORE1] = { .name = "system.core1", .label = "Processor load, core 1",
        .description = "Load of processor core 1.",
        .unit = "%", .part = "system", .period = 60, .monitor = true, .chart = true,
        .has_min = true, .min = 0, .has_max = true, .max = 100, .has_high = true, .high = 90, .decimals = 1 },
    [SYS_MEM_FREE] = { .name = "system.memory.free", .label = "Free memory",
        .description = "Free internal memory.",
        .unit = "B", .part = "system", .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 },
    [SYS_MEM_MIN] = { .name = "system.memory.min_free", .label = "Lowest free memory",
        .description = "Smallest amount of free internal memory since the gateway started.",
        .unit = "B", .part = "system", .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 },
    [SYS_MEM_LARGEST] = { .name = "system.memory.largest", .label = "Largest free block",
        .description = "Largest block of internal memory that can be allocated now.",
        .unit = "B", .part = "system", .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 },
    [SYS_MEM_PCT] = { .name = "system.memory.pct", .label = "Memory used",
        .description = "Share of the internal memory in use.",
        .unit = "%", .part = "system", .period = 60, .monitor = true, .chart = true,
        .has_min = true, .min = 0, .has_max = true, .max = 100, .has_high = true, .high = 90, .decimals = 1 },
    [SYS_NVS_PCT] = { .name = "system.nvs.pct", .label = "Settings storage used",
        .description = "Share of the settings area in flash that is in use.",
        .unit = "%", .part = "system", .period = 60, .chart = true,
        .has_min = true, .min = 0, .has_max = true, .max = 100, .has_high = true, .high = 90, .decimals = 1 },
};

static const metric_def_t s_fam_up = { .name = "usage.mqtt.up.*", .label = "MQTT data sent per topic",
    .description = "Bytes of whole MQTT packets sent on one topic since the gateway started. One metric per topic, named without mower/<name>/.",
    .unit = "B", .part = "usage", .counter = true, .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 };
static const metric_def_t s_fam_down = { .name = "usage.mqtt.down.*", .label = "MQTT data received per topic",
    .description = "Bytes of whole MQTT packets received on one topic since the gateway started. One metric per topic, named without mower/<name>/.",
    .unit = "B", .part = "usage", .counter = true, .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 };

static double round1(double v)
{
    return floor(v * 10 + 0.5) / 10;
}

/* Processor load from the idle tasks' run time (FreeRTOS run-time statistics). */
static double s_cpu = NAN, s_core[2] = { NAN, NAN };

static void cpu_sample(void)
{
    static int64_t last_us;
    static uint32_t last_total;
    static uint32_t last_idle[2];
    static bool have_last;
    int64_t now = esp_timer_get_time();
    if (have_last && now - last_us < 50000) {
        return;   /* the cached reading is fresh enough */
    }
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = malloc(n * sizeof(TaskStatus_t));
    if (!tasks) {
        return;
    }
    configRUN_TIME_COUNTER_TYPE total = 0;
    n = uxTaskGetSystemState(tasks, n, &total);
    uint32_t idle[2] = { 0, 0 };
    for (UBaseType_t i = 0; i < n; i++) {
        if (strcmp(tasks[i].pcTaskName, "IDLE0") == 0) {
            idle[0] = (uint32_t)tasks[i].ulRunTimeCounter;
        } else if (strcmp(tasks[i].pcTaskName, "IDLE1") == 0) {
            idle[1] = (uint32_t)tasks[i].ulRunTimeCounter;
        }
    }
    free(tasks);
    uint32_t dt = (uint32_t)total - last_total;
    if (dt > 0) {
        double sum = 0;
        for (int c = 0; c < 2; c++) {
            double busy = 100.0 - (double)(uint32_t)(idle[c] - last_idle[c]) * 100.0 / dt;
            s_core[c] = round1(busy < 0 ? 0 : busy > 100 ? 100 : busy);
            sum += s_core[c];
        }
        s_cpu = round1(sum / 2);
    }
    last_total = (uint32_t)total;
    last_idle[0] = idle[0];
    last_idle[1] = idle[1];
    last_us = now;
    have_last = true;
}

static bool sys_read(void *ctx, double *v)
{
    switch ((sys_id_t)(intptr_t)ctx) {
    case SYS_CPU:
        cpu_sample();
        *v = s_cpu;
        return !isnan(s_cpu);
    case SYS_CORE0:
    case SYS_CORE1:
        cpu_sample();
        *v = s_core[(intptr_t)ctx == SYS_CORE0 ? 0 : 1];
        return !isnan(*v);
    case SYS_MEM_FREE:
        *v = (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        return true;
    case SYS_MEM_MIN:
        *v = (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        return true;
    case SYS_MEM_LARGEST:
        *v = (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        return true;
    case SYS_MEM_PCT: {
        size_t total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (!total) {
            return false;
        }
        *v = round1(100.0 * (double)(total - free_b) / (double)total);
        return true;
    }
    case SYS_NVS_PCT: {
        nvs_stats_t st;
        if (nvs_get_stats(NULL, &st) != ESP_OK || st.total_entries == 0) {
            return false;
        }
        *v = round1(100.0 * (double)st.used_entries / (double)st.total_entries);
        return true;
    }
    default:
        return false;
    }
}

/* ------------------------------------------------------------------- values */

bool metrics_read(metric_ref_t r, double *v)
{
    switch (r.type) {
    case 0:
        return r.idx >= 0 && r.idx < s_n && s_metrics[r.idx].fn(s_metrics[r.idx].ctx, v);
    case 1:
        *v = (double)usage_topic_up(r.idx);
        return true;
    default:
        *v = (double)usage_topic_down(r.idx);
        return true;
    }
}

bool metrics_rate_offered(int rate)
{
    for (size_t r = 0; r < N_RATES; r++) {
        if (rate == s_rates[r]) {
            return true;
        }
    }
    return false;
}

bool metrics_resolve(const char *name, metric_ref_t *out)
{
    for (int i = 0; i < s_n; i++) {
        if (!s_metrics[i].family && strcmp(s_metrics[i].def->name, name) == 0) {
            if (!s_metrics[i].def->monitor) {
                return false;
            }
            *out = (metric_ref_t){ 0, (int16_t)i };
            return true;
        }
    }
    int type = 0;
    const char *topic = NULL;
    if (strncmp(name, FAM_UP_PREFIX, strlen(FAM_UP_PREFIX)) == 0) {
        type = 1;
        topic = name + strlen(FAM_UP_PREFIX);
    } else if (strncmp(name, FAM_DOWN_PREFIX, strlen(FAM_DOWN_PREFIX)) == 0) {
        type = 2;
        topic = name + strlen(FAM_DOWN_PREFIX);
    }
    if (topic && topic[0]) {
        for (int i = 0; i < usage_topic_count(); i++) {
            if (strcmp(usage_topic_name(i), topic) == 0) {
                *out = (metric_ref_t){ (int8_t)type, (int16_t)i };
                return true;
            }
        }
    }
    return false;
}

/* --------------------------------------------------------------- catalogue */

static cJSON *entry(const metric_def_t *d)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", d->name);
    if (d->key) {
        cJSON_AddStringToObject(e, "key", d->key);
    }
    cJSON_AddStringToObject(e, "label", d->label);
    cJSON_AddStringToObject(e, "description", d->description);
    cJSON_AddStringToObject(e, "unit", d->unit);
    cJSON_AddStringToObject(e, "kind", d->counter ? "counter" : "gauge");
    cJSON_AddStringToObject(e, "type", "number");
    cJSON_AddNumberToObject(e, "period", d->period);
    cJSON *rates = cJSON_AddArrayToObject(e, "rates");
    if (d->monitor) {
        for (size_t i = 0; i < N_RATES; i++) {
            cJSON_AddItemToArray(rates, cJSON_CreateNumber(s_rates[i]));
        }
    }
    cJSON_AddBoolToObject(e, "chart", d->chart);
    if (d->has_min) {
        cJSON_AddNumberToObject(e, "min", d->min);
    }
    if (d->has_max) {
        cJSON_AddNumberToObject(e, "max", d->max);
    }
    if (d->has_low) {
        cJSON_AddNumberToObject(e, "low", d->low);
    }
    if (d->has_high) {
        cJSON_AddNumberToObject(e, "high", d->high);
    }
    cJSON_AddNumberToObject(e, "decimals", d->decimals);
    return e;
}

/* §16.1: parts first, then the index that lists them. Within a part, the
 * metrics come in the order they were registered. */
static void catalogue_publish(void)
{
    portENTER_CRITICAL(&s_mux);
    s_published = true;
    portEXIT_CRITICAL(&s_mux);
    for (size_t p = 0; p < N_PARTS; p++) {
        cJSON *part = cJSON_CreateObject();
        cJSON_AddNumberToObject(part, "revision", CATALOGUE_REVISION);
        cJSON_AddStringToObject(part, "label", s_part_labels[p]);
        cJSON *arr = cJSON_AddArrayToObject(part, "metrics");
        for (int i = 0; i < s_n; i++) {
            if (strcmp(s_metrics[i].def->part, s_parts[p]) == 0) {
                cJSON_AddItemToArray(arr, entry(s_metrics[i].def));
            }
        }
        char *json = cJSON_PrintUnformatted(part);
        cJSON_Delete(part);
        if (!json) {
            continue;
        }
        if (strlen(json) > 4096) {
            ESP_LOGE(TAG, "catalogue part %s is %u bytes, more than 4096", s_parts[p], (unsigned)strlen(json));
        }
        char sub[48];
        snprintf(sub, sizeof(sub), "metrics/%s", s_parts[p]);
        mqtt_link_publish(MQTT_SCOPE_GATEWAY, sub, json, 0, 1, true);
        free(json);
    }
    cJSON *idx = cJSON_CreateObject();
    cJSON_AddNumberToObject(idx, "revision", CATALOGUE_REVISION);
    cJSON *parts = cJSON_AddArrayToObject(idx, "parts");
    for (size_t p = 0; p < N_PARTS; p++) {
        cJSON_AddItemToArray(parts, cJSON_CreateString(s_parts[p]));
    }
    char *json = cJSON_PrintUnformatted(idx);
    cJSON_Delete(idx);
    if (json) {
        mqtt_link_publish(MQTT_SCOPE_GATEWAY, "metrics/index", json, 0, 1, true);
        free(json);
    }
}

/* --------------------------------------------------------------- telemetry */

static int64_t s_next_due_us[MAX_METRICS];

static void telemetry_reset(void)
{
    memset(s_next_due_us, 0, sizeof(s_next_due_us));   /* everything is due at once */
}

static void telemetry_poll(int64_t now)
{
    bool any = false;
    for (int i = 0; i < s_n; i++) {
        if (now >= s_next_due_us[i]) {
            any = true;
            break;
        }
    }
    if (!any) {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    gw_add_time(root, true);
    cJSON_AddNumberToObject(root, "boot", gw_boot());
    cJSON *m = cJSON_AddObjectToObject(root, "metrics");
    for (int i = 0; i < s_n; i++) {
        if (now < s_next_due_us[i]) {
            continue;
        }
        const metric_t *mt = &s_metrics[i];
        s_next_due_us[i] = now + (int64_t)mt->def->period * 1000000;
        if (!mt->family) {
            double v;
            if (mt->fn(mt->ctx, &v)) {
                cJSON_AddNumberToObject(m, mt->def->name, v);
            } else {
                cJSON_AddNullToObject(m, mt->def->name);
            }
        } else {
            bool up = mt->family == 1;
            for (int t = 0; t < usage_topic_count(); t++) {
                char name[80];
                snprintf(name, sizeof(name), "%s%s", up ? FAM_UP_PREFIX : FAM_DOWN_PREFIX, usage_topic_name(t));
                uint64_t b = up ? usage_topic_up(t) : usage_topic_down(t);
                if (b > 0) {   /* a topic appears once it has carried data */
                    cJSON_AddNumberToObject(m, name, (double)b);
                }
            }
        }
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return;
    }
    size_t len = strlen(json);
    if (len > 4096) {
        ESP_LOGE(TAG, "telemetry is %u bytes, more than 4096", (unsigned)len);
    } else if (mqtt_link_publish(MQTT_SCOPE_GATEWAY, "telemetry", json, 0, 0, false) == ESP_OK) {
        ESP_LOGI(TAG, "telemetry %u bytes: %.200s%s", (unsigned)len, json, len > 200 ? "..." : "");
    }
    free(json);
}

/* ------------------------------------------------------------------- tasks */

/* §4.2: when the server link comes up, the gateway publishes its catalogue
 * again (the supervisor publishes the state on the same edge). */
static void tick_task(void *arg)
{
    bool was_up = false;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));
        gw_task_alive(GW_T_TICK);
        int64_t now = esp_timer_get_time();
        bool up = gw_link_get() & GW_SERVER_LINK_UP;
        if (up && !was_up) {
            ESP_LOGI(TAG, "server link up: publishing the catalogue");
            catalogue_publish();
            telemetry_reset();
        }
        was_up = up;
        if (up) {
            telemetry_poll(now);
        }
        monitor_tick(now);
    }
}

/* The modem's signal is only read while PPP is paused for 2-3 s: once a minute,
 * never during an update. */
static void signal_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        gw_task_alive(GW_T_SIGNAL);
        if (cell_is_up() && !ota_busy()) {
            cell_refresh_signal();
        }
    }
}

void metrics_start(void)
{
    for (int i = 0; i < SYS_COUNT; i++) {
        metrics_register(&s_sys_defs[i], sys_read, (void *)(intptr_t)i);
    }
    add(&s_fam_up, NULL, NULL, 1);
    add(&s_fam_down, NULL, NULL, 2);
    if (monitor_init() != ESP_OK || gw_task_create(GW_T_TICK, tick_task, NULL, NULL) != pdPASS ||
            gw_task_create(GW_T_SIGNAL, signal_task, NULL, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the publisher tasks");
    }
}
