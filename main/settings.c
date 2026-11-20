#include "settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout unusable (%s): erasing it", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

esp_err_t settings_get_str(const char *ns, const char *key, char *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_str(h, key, buf, &len);
        nvs_close(h);
    }
    return err;
}

esp_err_t settings_get_u32(const char *ns, const char *key, uint32_t *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u32(h, key, val);
        nvs_close(h);
    }
    return err;
}

esp_err_t settings_get_blob(const char *ns, const char *key, void *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_blob(h, key, buf, len);
        nvs_close(h);
    }
    return err;
}

typedef enum { W_STR, W_U32, W_BLOB, W_ERASE } wkind_t;

static esp_err_t put(nvs_handle_t h, wkind_t kind, const char *key, const void *val, size_t len)
{
    switch (kind) {
    case W_STR:
        return nvs_set_str(h, key, val);
    case W_U32:
        return nvs_set_u32(h, key, *(const uint32_t *)val);
    case W_BLOB:
        return nvs_set_blob(h, key, val, len);
    case W_ERASE: {
        esp_err_t err = nvs_erase_key(h, key);
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    }
    return ESP_ERR_INVALID_ARG;
}

/* One key: open, write, commit. Logs namespace and key only, never the value. */
static esp_err_t write1(const char *ns, wkind_t kind, const char *key, const void *val, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = put(h, kind, key, val, len);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "writing %s/%s failed: %s", ns, key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_set_str(const char *ns, const char *key, const char *val)
{
    return write1(ns, W_STR, key, val, 0);
}

esp_err_t settings_set_u32(const char *ns, const char *key, uint32_t val)
{
    return write1(ns, W_U32, key, &val, 0);
}

esp_err_t settings_set_blob(const char *ns, const char *key, const void *val, size_t len)
{
    return write1(ns, W_BLOB, key, val, len);
}

esp_err_t settings_erase(const char *ns, const char *key)
{
    return write1(ns, W_ERASE, key, NULL, 0);
}

esp_err_t settings_set_many(const char *ns, const settings_kv_t *kv, int n)
{
    nvs_handle_t h;
    const char *key = "";
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        for (int i = 0; err == ESP_OK && i < n; i++) {
            key = kv[i].key;
            err = put(h, kv[i].val ? W_STR : W_ERASE, key, kv[i].val, 0);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "writing %s/%s failed: %s", ns, key, esp_err_to_name(err));
    }
    return err;
}
