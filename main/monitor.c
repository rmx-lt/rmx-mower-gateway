#include "monitor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gw_link.h"
#include "gwstate.h"
#include "metrics.h"
#include "mqtt_link.h"

static const char *TAG = "monitor";

#define MONITOR_BUDGET     100    /* values per second, wiki Mower-Protocol §15 */
#define MON_MAX_METRICS    16
#define MON_BATCH          4
#define MON_FLUSH_US       200000
#define MON_HOLD_DEFAULT   10
#define MON_HOLD_MAX       60

typedef struct stream {
    struct stream *next;
    char session[65];
    int n;
    metric_ref_t ref[MON_MAX_METRICS];
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

/* Running streams. There is no fixed number of them: the budget is the only limit
 * (wiki Mower-Protocol §15); a stream costs one allocation. */
static stream_t *s_streams;
static SemaphoreHandle_t s_mon_lock;

/* Unlinks and frees the stream that *link points to; *link then points to the next. */
static void stream_remove(stream_t **link)
{
    stream_t *s = *link;
    *link = s->next;
    for (int i = 0; i < s->n; i++) {
        free(s->names[i]);
    }
    free(s);
}

/* Caller holds s_mon_lock. */
static int budget_used(void)
{
    int used = 0;
    for (const stream_t *s = s_streams; s; s = s->next) {
        used += s->n * s->rate;
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
    metric_ref_t refs[MON_MAX_METRICS];

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
            if (!name || !metrics_resolve(name, &refs[i])) {
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
        bool rate_ok = metrics_rate_offered(rate);
        if (names_ok && !rate_ok) {
            *message = "a metric asked for does not offer this rate (1, 2, 5 or 10)";
        } else if (names_ok) {
            stream_t *s = s_streams;
            while (s && strcmp(s->session, session) != 0) {
                s = s->next;
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
            } else if (budget_used() + n * rate > MONITOR_BUDGET) {
                *error = "over_budget";
                *message = "the stream does not fit the budget of 100 values per second";
            } else if (!(s = calloc(1, sizeof(*s)))) {
                *error = "failed";
                *message = "out of memory";
            } else {
                snprintf(s->session, sizeof(s->session), "%s", session);
                s->n = n;
                int k = 0;
                cJSON_ArrayForEach(it, names) {
                    s->names[k] = strdup(cJSON_GetStringValue(it));
                    s->ref[k] = refs[k];
                    k++;
                }
                s->rate = rate;
                s->period_us = 1000000 / rate;
                s->next_us = now;
                s->deadline_us = now + (int64_t)(hold * 1e6);
                s->next = s_streams;
                s_streams = s;
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
    stream_t **link = &s_streams;
    while (*link) {
        if (!session || strcmp((*link)->session, session) == 0) {
            ESP_LOGI(TAG, "monitor %s stopped", (*link)->session);
            stream_remove(link);
        } else {
            link = &(*link)->next;
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
        mqtt_link_publish(MQTT_SCOPE_GATEWAY, "monitor", json, 0, 0, false);
        free(json);
    }
    s->count = 0;
}

void monitor_tick(int64_t now)
{
    xSemaphoreTake(s_mon_lock, portMAX_DELAY);
    bool lost = gw_link_get() & GW_SERVER_LOST;
    for (stream_t **link = &s_streams; *link;) {
        stream_t *s = *link;
        if (lost || now > s->deadline_us) {
            ESP_LOGI(TAG, "monitor %s ended (%s)", s->session, lost ? "server link lost" : "hold ran out");
            stream_remove(link);
            continue;
        }
        link = &s->next;
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
                s->vals[s->count][m] = metrics_read(s->ref[m], &v) ? v : NAN;
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

esp_err_t monitor_init(void)
{
    s_mon_lock = xSemaphoreCreateMutex();
    return s_mon_lock ? ESP_OK : ESP_ERR_NO_MEM;
}
