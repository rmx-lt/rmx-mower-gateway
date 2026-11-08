#include "creds.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "creds";

esp_err_t creds_get(const char *key, char *buf, size_t len)
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
        ESP_LOGE(TAG, "no '%s' in NVS (%s): run tools/provision_nvs.ps1", key, esp_err_to_name(err));
    }
    return err;
}
