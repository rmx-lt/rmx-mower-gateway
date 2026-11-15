#include "metrics.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cell.h"
#include "gwstate.h"
#include "mqtt_link.h"
#include "ota.h"
#include "usage.h"

static const char *TAG = "metrics";

/* Bump when the catalogue changes (names, units, periods, rates, ...). */
#define CATALOGUE_REVISION 1

#define TICK_MS            100
#define MONITOR_BUDGET     100    /* values per second, wiki Mower-Protocol §15 */
#define MON_MAX_STREAMS    8
#define MON_MAX_METRICS    16
#define MON_BATCH          4
#define MON_FLUSH_US       200000
#define MON_HOLD_DEFAULT   10
#define MON_HOLD_MAX       60
#define STATE_CHECK_US     1000000

/* ---------------------------------------------------------------- catalogue */

typedef enum {
    M_SIGNAL,
    M_TOTAL_UP, M_TOTAL_DOWN,
    M_CPU, M_CORE0, M_CORE1,
    M_MEM_FREE, M_MEM_MIN, M_MEM_LARGEST, M_MEM_PCT,
    M_NVS_PCT,
    M_COUNT,
    M_FAM_UP = M_COUNT,   /* usage.mqtt.up.*   (family) */
    M_FAM_DOWN,           /* usage.mqtt.down.* (family) */
    M_DEFS
} metric_id_t;

typedef struct {
    const char *name;
    const char *key;
    const char *label;
    const char *description;
    const char *unit;
    const char *part;
    bool counter;
    int period;       /* seconds in telemetry */
    bool monitor;     /* offers the monitor rates */
    bool chart;
    bool has_min, has_max, has_low, has_high;
    double min, max, low, high;
    int decimals;
} metric_def_t;

/* Monitor rates (samples per second) the monitorable metrics offer. */
static const int s_rates[] = { 1, 2, 5, 10 };
#define N_RATES (sizeof(s_rates) / sizeof(s_rates[0]))

static const char *const s_parts[] = { "signal", "usage", "system" };
static const char *const s_part_labels[] = { "Signal", "Data usage", "System" };
#define N_PARTS (sizeof(s_parts) / sizeof(s_parts[0]))

static const metric_def_t s_defs[M_DEFS] = {
    /* The modem's signal can only be read with PPP paused, about once a minute,
     * so it offers no monitor rates. */
    [M_SIGNAL] = { .name = "signal.rssi", .key = "signal.strength", .label = "Signal strength",
        .description = "Received signal strength of the mobile connection. Read once a minute.",
        .unit = "dBm", .part = "signal", .period = 60, .chart = true,
        .has_min = true, .min = -120, .has_max = true, .max = -50, .has_low = true, .low = -100, .decimals = 0 },
    [M_TOTAL_UP] = { .name = "usage.total.up", .label = "Data sent",
        .description = "Bytes sent on the mobile connection since the gateway started, as counted on the network interface. The SIM allowance is charged for this.",
        .unit = "B", .part = "usage", .counter = true, .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 },
    [M_TOTAL_DOWN] = { .name = "usage.total.down", .label = "Data received",
        .description = "Bytes received on the mobile connection since the gateway started, as counted on the network interface.",
        .unit = "B", .part = "usage", .counter = true, .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 },
    [M_CPU] = { .name = "system.cpu", .label = "Processor load",
        .description = "Average load of both processor cores.",
        .unit = "%", .part = "system", .period = 60, .monitor = true, .chart = true,
        .has_min = true, .min = 0, .has_max = true, .max = 100, .has_high = true, .high = 90, .decimals = 1 },
    [M_CORE0] = { .name = "system.core0", .label = "Processor load, core 0",
        .description = "Load of processor core 0.",
        .unit = "%", .part = "system", .period = 60, .monitor = true, .chart = true,
        .has_min = true, .min = 0, .has_max = true, .max = 100, .has_high = true, .high = 90, .decimals = 1 },
    [M_CORE1] = { .name = "system.core1", .label = "Processor load, core 1",
        .description = "Load of processor core 1.",
        .unit = "%", .part = "system", .period = 60, .monitor = true, .chart = true,
        .has_min = true, .min = 0, .has_max = true, .max = 100, .has_high = true, .high = 90, .decimals = 1 },
    [M_MEM_FREE] = { .name = "system.memory.free", .label = "Free memory",
        .description = "Free internal memory.",
        .unit = "B", .part = "system", .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 },
    [M_MEM_MIN] = { .name = "system.memory.min_free", .label = "Lowest free memory",
        .description = "Smallest amount of free internal memory since the gateway started.",
        .unit = "B", .part = "system", .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 },
    [M_MEM_LARGEST] = { .name = "system.memory.largest", .label = "Largest free block",
        .description = "Largest block of internal memory that can be allocated now.",
        .unit = "B", .part = "system", .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 },
    [M_MEM_PCT] = { .name = "system.memory.pct", .label = "Memory used",
        .description = "Share of the internal memory in use.",
        .unit = "%", .part = "system", .period = 60, .monitor = true, .chart = true,
        .has_min = true, .min = 0, .has_max = true, .max = 100, .has_high = true, .high = 90, .decimals = 1 },
    [M_NVS_PCT] = { .name = "system.nvs.pct", .label = "Settings storage used",
        .description = "Share of the settings area in flash that is in use.",
        .unit = "%", .part = "system", .period = 60, .chart = true,
        .has_min = true, .min = 0, .has_max = true, .max = 100, .has_high = true, .high = 90, .decimals = 1 },
    [M_FAM_UP] = { .name = "usage.mqtt.up.*", .label = "MQTT data sent per topic",
        .description = "Bytes of whole MQTT packets sent on one topic since the gateway started. One metric per topic, named without mower/<name>/.",
        .unit = "B", .part = "usage", .counter = true, .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 },
    [M_FAM_DOWN] = { .name = "usage.mqtt.down.*", .label = "MQTT data received per topic",
        .description = "Bytes of whole MQTT packets received on one topic since the gateway started. One metric per topic, named without mower/<name>/.",
        .unit = "B", .part = "usage", .counter = true, .period = 60, .monitor = true, .chart = true, .has_min = true, .decimals = 0 },
};

#define FAM_UP_PREFIX   "usage.mqtt.up."
#define FAM_DOWN_PREFIX "usage.mqtt.down."

/* ------------------------------------------------------------------- values */

typedef struct {
    int8_t type;     /* 0 static metric (idx = metric_id_t), 1 up family member, 2 down family member */
    int16_t idx;
} mref_t;

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

/* False: the value is not known (null). */
static bool static_value(int id, double *v)
{
    switch (id) {
    case M_SIGNAL: {
        int csq = cell_last_rssi();
        if (csq == 99) {
            return false;
        }
        *v = -113 + 2 * csq;
        return true;
    }
    case M_TOTAL_UP:
        *v = (double)usage_total_up();
        return true;
    case M_TOTAL_DOWN:
        *v = (double)usage_total_down();
        return true;
    case M_CPU:
        cpu_sample();
        *v = s_cpu;
        return !isnan(s_cpu);
    case M_CORE0:
    case M_CORE1:
        cpu_sample();
        *v = s_core[id == M_CORE0 ? 0 : 1];
        return !isnan(*v);
    case M_MEM_FREE:
        *v = (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        return true;
    case M_MEM_MIN:
        *v = (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        return true;
    case M_MEM_LARGEST:
        *v = (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        return true;
    case M_MEM_PCT: {
        size_t total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (!total) {
            return false;
        }
        *v = round1(100.0 * (double)(total - free_b) / (double)total);
        return true;
    }
    case M_NVS_PCT: {
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

static bool ref_value(mref_t r, double *v)
{
    switch (r.type) {
    case 0:
        return static_value(r.idx, v);
    case 1:
        *v = (double)usage_topic_up(r.idx);
        return true;
    default:
        *v = (double)usage_topic_down(r.idx);
        return true;
    }
}

/* Name -> reference for monitor requests. Family members must already exist. */
static bool resolve(const char *name, mref_t *out)
{
    for (int i = 0; i < M_COUNT; i++) {
        if (strcmp(s_defs[i].name, name) == 0) {
            if (!s_defs[i].monitor) {
                return false;
            }
            *out = (mref_t){ 0, (int16_t)i };
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
                *out = (mref_t){ (int8_t)type, (int16_t)i };
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

/* §16.1: parts first, then the index that lists them. */
static void catalogue_publish(void)
{
    for (size_t p = 0; p < N_PARTS; p++) {
        cJSON *part = cJSON_CreateObject();
        cJSON_AddNumberToObject(part, "revision", CATALOGUE_REVISION);
        cJSON_AddStringToObject(part, "label", s_part_labels[p]);
        cJSON *arr = cJSON_AddArrayToObject(part, "metrics");
        for (int i = 0; i < M_DEFS; i++) {
            if (strcmp(s_defs[i].part, s_parts[p]) == 0) {
                cJSON_AddItemToArray(arr, entry(&s_defs[i]));
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
        mqtt_link_publish(sub, json, 0, 1, true);
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
        mqtt_link_publish("metrics/index", json, 0, 1, true);
        free(json);
    }
}

/* --------------------------------------------------------------- telemetry */

static int64_t s_next_due_us[M_DEFS];

static void telemetry_reset(void)
{
    memset(s_next_due_us, 0, sizeof(s_next_due_us));   /* everything is due at once */
}

static void telemetry_poll(int64_t now)
{
    bool any = false;
    for (int i = 0; i < M_DEFS; i++) {
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
    for (int i = 0; i < M_DEFS; i++) {
        if (now < s_next_due_us[i]) {
            continue;
        }
        s_next_due_us[i] = now + (int64_t)s_defs[i].period * 1000000;
        if (i < M_COUNT) {
            double v;
            if (static_value(i, &v)) {
                cJSON_AddNumberToObject(m, s_defs[i].name, v);
            } else {
                cJSON_AddNullToObject(m, s_defs[i].name);
            }
        } else {
            bool up = i == M_FAM_UP;
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
    } else if (mqtt_link_publish("telemetry", json, 0, 0, false) == ESP_OK) {
        ESP_LOGI(TAG, "telemetry %u bytes: %.200s%s", (unsigned)len, json, len > 200 ? "..." : "");
    }
    free(json);
}

/* ------------------------------------------------------------------ monitor */

typedef struct {
    bool used;
    char session[65];
    int n;
    mref_t ref[MON_MAX_METRICS];
    char *names[MON_MAX_METRICS];
    int rate;
    int64_t period_us;
    int64_t next_us;          /* when the next sample is due */
    int64_t deadline_us;      /* the stream ends here unless extended */
    uint32_t index;           /* number of the next sample */
    int count;                /* samples waiting in the batch */
    uint32_t batch_index;     /* number of the batch's first sample */
    int64_t batch_start_us;   /* when the batch's first sample was due */
    double vals[MON_BATCH][MON_MAX_METRICS];
} stream_t;

static stream_t s_streams[MON_MAX_STREAMS];
static SemaphoreHandle_t s_mon_lock;

static void stream_free(stream_t *s)
{
    for (int i = 0; i < s->n; i++) {
        free(s->names[i]);
    }
    memset(s, 0, sizeof(*s));
}

/* Caller holds s_mon_lock. */
static int budget_used(void)
{
    int used = 0;
    for (int i = 0; i < MON_MAX_STREAMS; i++) {
        if (s_streams[i].used) {
            used += s_streams[i].n * s_streams[i].rate;
        }
    }
    return used;
}

static void fill_budget(cJSON *result)
{
    cJSON_AddNumberToObject(result, "budget", MONITOR_BUDGET);
    cJSON_AddNumberToObject(result, "used", budget_used());
}

static bool valid_session(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n < 1 || n > 64) {
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

bool monitor_start(const cJSON *cmd, cJSON *result, const char **error, const char **message)
{
    const char *session = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "session"));
    const cJSON *names = cJSON_GetObjectItem(cmd, "metrics");
    const cJSON *rate_item = cJSON_GetObjectItem(cmd, "rate");
    const cJSON *hold_item = cJSON_GetObjectItem(cmd, "hold");
    bool ok = false;

    xSemaphoreTake(s_mon_lock, portMAX_DELAY);
    *error = "bad_request";
    int n = cJSON_IsArray(names) ? cJSON_GetArraySize(names) : 0;
    int rate = cJSON_IsNumber(rate_item) ? (int)cJSON_GetNumberValue(rate_item) : 0;
    double hold = MON_HOLD_DEFAULT;
    mref_t refs[MON_MAX_METRICS];

    if (!valid_session(session)) {
        *message = "session must be an id (1-64 chars of A-Za-z0-9_-)";
    } else if (n < 1 || n > MON_MAX_METRICS) {
        *message = "metrics must list 1 to 16 metric names";
    } else if (!cJSON_IsNumber(rate_item) || rate != cJSON_GetNumberValue(rate_item)) {
        *message = "rate must be a whole number of samples per second";
    } else if (hold_item && (!cJSON_IsNumber(hold_item) ||
                             (hold = cJSON_GetNumberValue(hold_item)) < 1 || hold > MON_HOLD_MAX)) {
        *message = "hold must be 1 to 60 seconds";
    } else {
        bool names_ok = true;
        int i = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, names) {
            const char *name = cJSON_GetStringValue(it);
            if (!name || !resolve(name, &refs[i])) {
                *message = "unknown metric, or one that cannot be monitored (family names with * are not accepted)";
                names_ok = false;
                break;
            }
            for (int j = 0; j < i; j++) {
                if (refs[j].type == refs[i].type && refs[j].idx == refs[i].idx) {
                    *message = "a metric is listed twice";
                    names_ok = false;
                }
            }
            i++;
        }
        bool rate_ok = false;
        for (size_t r = 0; r < N_RATES; r++) {
            rate_ok = rate_ok || rate == s_rates[r];
        }
        if (names_ok && !rate_ok) {
            *message = "a metric asked for does not offer this rate (1, 2, 5 or 10)";
        } else if (names_ok) {
            stream_t *s = NULL;
            stream_t *freeslot = NULL;
            for (int k = 0; k < MON_MAX_STREAMS; k++) {
                if (s_streams[k].used && strcmp(s_streams[k].session, session) == 0) {
                    s = &s_streams[k];
                } else if (!s_streams[k].used && !freeslot) {
                    freeslot = &s_streams[k];
                }
            }
            int64_t now = esp_timer_get_time();
            if (s) {
                bool same = s->n == n && s->rate == rate;
                for (int k = 0; same && k < n; k++) {
                    same = s->ref[k].type == refs[k].type && s->ref[k].idx == refs[k].idx;
                }
                if (!same) {
                    *message = "this session is running with other metrics or another rate";
                } else {
                    s->deadline_us = now + (int64_t)(hold * 1e6);   /* extends it, the sample index goes on */
                    ok = true;
                }
            } else if (!freeslot || budget_used() + n * rate > MONITOR_BUDGET) {
                *error = "over_budget";
                *message = freeslot ? "the stream does not fit the budget of 100 values per second"
                                    : "too many streams are running";
            } else {
                memset(freeslot, 0, sizeof(*freeslot));
                freeslot->used = true;
                snprintf(freeslot->session, sizeof(freeslot->session), "%s", session);
                freeslot->n = n;
                int k = 0;
                cJSON_ArrayForEach(it, names) {
                    freeslot->names[k] = strdup(cJSON_GetStringValue(it));
                    freeslot->ref[k] = refs[k];
                    k++;
                }
                freeslot->rate = rate;
                freeslot->period_us = 1000000 / rate;
                freeslot->next_us = now;
                freeslot->deadline_us = now + (int64_t)(hold * 1e6);
                ok = true;
                ESP_LOGI(TAG, "monitor %s: %d metrics at %d/s for %.0f s", session, n, rate, hold);
            }
        }
    }
    fill_budget(result);
    xSemaphoreGive(s_mon_lock);
    return ok;
}

void monitor_stop(const cJSON *cmd, cJSON *result)
{
    const char *session = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "session"));
    xSemaphoreTake(s_mon_lock, portMAX_DELAY);
    for (int i = 0; i < MON_MAX_STREAMS; i++) {
        if (s_streams[i].used && (!session || strcmp(s_streams[i].session, session) == 0)) {
            ESP_LOGI(TAG, "monitor %s stopped", s_streams[i].session);
            stream_free(&s_streams[i]);
        }
    }
    fill_budget(result);
    xSemaphoreGive(s_mon_lock);
}

static void stream_flush(stream_t *s, int64_t now)
{
    if (s->count == 0) {
        return;
    }
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "session", s->session);
    cJSON_AddNumberToObject(msg, "index", s->batch_index);
    double wall = gw_time();
    if (wall < 0) {
        cJSON_AddNullToObject(msg, "time");
    } else {
        double t = wall - (double)(now - s->batch_start_us) / 1e6;
        cJSON_AddNumberToObject(msg, "time", floor(t * 1000) / 1000);
    }
    cJSON_AddNumberToObject(msg, "rate", s->rate);
    cJSON_AddItemToObject(msg, "metrics", cJSON_CreateStringArray((const char *const *)s->names, s->n));
    cJSON *samples = cJSON_AddArrayToObject(msg, "samples");
    for (int k = 0; k < s->count; k++) {
        cJSON *row = cJSON_CreateArray();
        for (int m = 0; m < s->n; m++) {
            double v = s->vals[k][m];
            cJSON_AddItemToArray(row, isnan(v) ? cJSON_CreateNull() : cJSON_CreateNumber(v));
        }
        cJSON_AddItemToArray(samples, row);
    }
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (json) {
        mqtt_link_publish("monitor", json, 0, 0, false);
        free(json);
    }
    s->count = 0;
}

static void monitor_tick(int64_t now)
{
    xSemaphoreTake(s_mon_lock, portMAX_DELAY);
    bool lost = mqtt_link_server_lost();
    for (int i = 0; i < MON_MAX_STREAMS; i++) {
        stream_t *s = &s_streams[i];
        if (!s->used) {
            continue;
        }
        if (lost || now > s->deadline_us) {
            ESP_LOGI(TAG, "monitor %s ended (%s)", s->session, lost ? "server link lost" : "hold ran out");
            stream_free(s);
            continue;
        }
        if (!mqtt_link_connected()) {
            s->next_us = now;   /* nothing is kept back for later */
            s->count = 0;
            continue;
        }
        if (now - s->next_us > 1000000) {
            s->next_us = now;   /* a long stall: do not replay it */
            s->count = 0;
        }
        while (now >= s->next_us) {
            if (s->count == 0) {
                s->batch_index = s->index;
                s->batch_start_us = s->next_us;
            }
            for (int m = 0; m < s->n; m++) {
                double v;
                s->vals[s->count][m] = ref_value(s->ref[m], &v) ? v : NAN;
            }
            s->count++;
            s->index++;
            s->next_us += s->period_us;
            if (s->count == MON_BATCH) {
                stream_flush(s, now);
            }
        }
        /* one message per 0.2 s of samples, or per sample when they are further apart */
        if (s->count > 0 && (s->count * s->period_us >= MON_FLUSH_US || now - s->batch_start_us >= 2 * MON_FLUSH_US)) {
            stream_flush(s, now);
        }
    }
    xSemaphoreGive(s_mon_lock);
}

/* ------------------------------------------------------------------- tasks */

/* §4.2: when the server link comes up, the gateway publishes its state and
 * its catalogue again (and the final ack of an update that restarted it). */
static void tick_task(void *arg)
{
    bool was_up = false;
    bool ota_reported = false;
    int64_t state_check_us = 0;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));
        mqtt_link_update();
        int64_t now = esp_timer_get_time();
        bool up = mqtt_link_server_up();
        if (up && !was_up) {
            ESP_LOGI(TAG, "server link up: publishing state and catalogue");
            ota_confirm();
            gw_state_publish(true);
            catalogue_publish();
            telemetry_reset();
            ota_reported = false;
            state_check_us = now;
        }
        was_up = up;
        if (up) {
            if (now - state_check_us >= STATE_CHECK_US) {
                state_check_us = now;
                gw_state_publish(false);   /* only when a field changed */
                if (!ota_reported) {
                    ota_reported = ota_report_result();   /* the final ack of an update that restarted us */
                }
            }
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
        if (cell_is_up() && !ota_busy()) {
            cell_refresh_signal();
        }
    }
}

void metrics_start(void)
{
    s_mon_lock = xSemaphoreCreateMutex();
    if (!s_mon_lock || xTaskCreate(tick_task, "gw_tick", 6144, NULL, 3, NULL) != pdPASS ||
            xTaskCreate(signal_task, "gw_signal", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the publisher tasks");
    }
}
