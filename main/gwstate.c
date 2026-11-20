#include "gwstate.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "cell.h"
#include "mqtt_link.h"

static const char *TAG = "state";

#define NVS_NS "gw"

static uint32_t s_boot;
static char *s_last_state;   /* last published state, without "time" */

void gw_boot_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "cannot open NVS namespace %s, boot counter stays 0", NVS_NS);
        return;
    }
    uint32_t boot = 0;
    nvs_get_u32(h, "boot", &boot);
    s_boot = boot + 1;
    nvs_set_u32(h, "boot", s_boot);
    nvs_commit(h);
    nvs_close(h);
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
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = len;
    bool ok = nvs_get_str(h, key, buf, &n) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool gw_nvs_set(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_str(h, key, val) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void gw_nvs_erase(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
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
    /* The autopilot link and the NTRIP client are other issues: until they exist
     * the facts are "no heartbeat" and "no settings". */
    cJSON_AddStringToObject(st, "autopilot_link", "down");
    cJSON_AddStringToObject(st, "ntrip", "off");
    const char *op = cell_operator();
    if (op[0]) {
        cJSON_AddStringToObject(st, "operator", op);
    } else {
        cJSON_AddNullToObject(st, "operator");
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
