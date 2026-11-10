#include "creds.h"
#include "devcert.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "creds";

static esp_err_t read_str(const char *key, char *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("creds", NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t n = len;
        err = nvs_get_str(h, key, buf, &n);
        nvs_close(h);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        buf[0] = '\0';
    }
    return err;
}

esp_err_t creds_get(const char *key, char *buf, size_t len)
{
    esp_err_t err = read_str(key, buf, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no '%s' in NVS (%s): run tools/provision_nvs.ps1", key, esp_err_to_name(err));
    }
    return err;
}

/* The name goes into MQTT topics and logins: no '/', '+', '#' or spaces. */
static bool valid_name(const char *s)
{
    if (!s[0]) {
        return false;
    }
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

const char *creds_device_name(void)
{
    static char s_name[CREDS_DEVICE_MAX + 1];
    if (s_name[0]) {
        return s_name;
    }
    /* A client certificate decides the name: the broker takes the identity from its CN. */
    const char *cn = devcert_cn();
    if (cn) {
        snprintf(s_name, sizeof(s_name), "%s", cn);
        char nvs_name[CREDS_DEVICE_MAX + 1];
        if (read_str(CREDS_DEVICE, nvs_name, sizeof(nvs_name)) == ESP_OK && strcmp(nvs_name, cn) != 0) {
            ESP_LOGW(TAG, "NVS '%s' (%s) differs from the certificate CN, using the CN", CREDS_DEVICE, nvs_name);
        }
        ESP_LOGI(TAG, "device name %s (from the client certificate)", s_name);
        return s_name;
    }
    esp_err_t err = read_str(CREDS_DEVICE, s_name, sizeof(s_name));
    if (err == ESP_OK && valid_name(s_name)) {
        ESP_LOGI(TAG, "device name %s (from NVS)", s_name);
        return s_name;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "no '%s' in NVS", CREDS_DEVICE);
    } else {
        ESP_LOGE(TAG, "'%s' in NVS is unusable (%s)", CREDS_DEVICE,
                 err == ESP_OK ? "only letters, digits, '-', '_'" : esp_err_to_name(err));
    }
    snprintf(s_name, sizeof(s_name), "%s", CONFIG_PROBE_DEVICE_NAME);
    ESP_LOGW(TAG, "device name %s (Kconfig default)", s_name);
    return s_name;
}
