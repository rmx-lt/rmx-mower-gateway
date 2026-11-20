#include "gwstate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "mqtt_link.h"
#include "settings.h"

static const char *TAG = "state";

#define NVS_NS "gw"

static uint32_t s_boot;
static char *s_last_state;   /* last published state, without "time" */

/* Module-owned fields, in publish order. Until the board link and the NTRIP
 * client exist the facts are "no heartbeat" and "no settings". */
#define MAX_FIELDS 8
typedef struct {
    char name[24];
    char value[32];
    bool null;
} field_t;
static field_t s_fields[MAX_FIELDS] = {
    { "autopilot_link", "down", false },
    { "ntrip", "off", false },
    { "operator", "", true },
};
static int s_nfields = 3;
static portMUX_TYPE s_fields_mux = portMUX_INITIALIZER_UNLOCKED;

void gw_boot_init(void)
{
    uint32_t boot = 0;
    settings_get_u32(NVS_NS, "boot", &boot);   /* NOT_FOUND on the first boot: 0 */
    s_boot = boot + 1;
    settings_set_u32(NVS_NS, "boot", s_boot);
    ESP_LOGI(TAG, "boot %lu", (unsigned long)s_boot);
}

uint32_t gw_boot(void)
{
    return s_boot;
}

double gw_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < 1700000000) {
        return -1;
    }
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

bool gw_clock_valid(void)
{
    return gw_time() >= 0;
}

void gw_add_time(cJSON *obj, bool fraction)
{
    double t = gw_time();
    if (t < 0) {
        cJSON_AddNullToObject(obj, "time");
    } else {
        cJSON_AddNumberToObject(obj, "time", fraction ? ((double)(int64_t)(t * 100)) / 100 : (double)(int64_t)t);
    }
}

bool gw_nvs_get(const char *key, char *buf, size_t len)
{
    return settings_get_str(NVS_NS, key, buf, len) == ESP_OK;
}

bool gw_nvs_set(const char *key, const char *val)
{
    return settings_set_str(NVS_NS, key, val) == ESP_OK;
}

void gw_nvs_erase(const char *key)
{
    settings_erase(NVS_NS, key);
}

bool gw_nvs_set_many(const settings_kv_t *kv, int n)
{
    return settings_set_many(NVS_NS, kv, n) == ESP_OK;
}

void gw_state_set(const char *field, const char *value)
{
    portENTER_CRITICAL(&s_fields_mux);
    int i = 0;
    while (i < s_nfields && strcmp(s_fields[i].name, field) != 0) {
        i++;
    }
    if (i == s_nfields && s_nfields < MAX_FIELDS) {
        snprintf(s_fields[i].name, sizeof(s_fields[i].name), "%s", field);
        s_nfields++;
    }
    if (i < s_nfields) {
        snprintf(s_fields[i].value, sizeof(s_fields[i].value), "%s", value ? value : "");
        s_fields[i].null = !value;
    }
    portEXIT_CRITICAL(&s_fields_mux);
}

void gw_state_publish(bool force)
{
    const char *version = esp_app_get_description()->version;
    char prev[32], prev_of[32];

    cJSON *st = cJSON_CreateObject();
    cJSON_AddStringToObject(st, "protocol", GW_PROTOCOL);
    cJSON_AddNumberToObject(st, "boot", s_boot);
    cJSON_AddStringToObject(st, "firmware", version);
    if (gw_nvs_get("prev", prev, sizeof(prev)) && gw_nvs_get("prev_of", prev_of, sizeof(prev_of)) &&
            strcmp(prev_of, version) == 0) {
        cJSON_AddStringToObject(st, "previous", prev);
    } else {
        cJSON_AddNullToObject(st, "previous");
    }
    cJSON_AddStringToObject(st, "partition", esp_ota_get_running_partition()->label);
    field_t fields[MAX_FIELDS];
    portENTER_CRITICAL(&s_fields_mux);
    int n = s_nfields;
    memcpy(fields, s_fields, sizeof(fields));
    portEXIT_CRITICAL(&s_fields_mux);
    for (int i = 0; i < n; i++) {
        if (fields[i].null) {
            cJSON_AddNullToObject(st, fields[i].name);
        } else {
            cJSON_AddStringToObject(st, fields[i].name, fields[i].value);
        }
    }

    char *body = cJSON_PrintUnformatted(st);
    if (!body) {
        cJSON_Delete(st);
        return;
    }
    if (force || !s_last_state || strcmp(body, s_last_state) != 0) {
        gw_add_time(st, false);
        char *full = cJSON_PrintUnformatted(st);
        if (full && mqtt_link_publish(MQTT_SCOPE_GATEWAY, "state", full, 0, 1, true) == ESP_OK) {
            free(s_last_state);
            s_last_state = body;
            body = NULL;
        }
        free(full);
    }
    free(body);
    cJSON_Delete(st);
}
