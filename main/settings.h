/* The one owner of NVS (wiki Mower-Gateway-Architecture §5.6). Namespaces and
 * keys in use: "gw" (boot counter, OTA state, previous version, legacy flag;
 * gwstate's gw_nvs_*) and "creds" (device key, certificate chain, fallback
 * passwords; devcert, creds).
 *
 * The module never logs a value, only namespace and key, so secrets (the
 * device key, the NTRIP password of #63) are safe to pass through it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "nvs.h"   /* ESP_ERR_NVS_* codes */

/* The one nvs_flash_init (nvs_flash_secure_init with #49). Erases the NVS
 * partition only when its layout is unusable (no free pages, new version),
 * as before. First thing in app_main. */
esp_err_t settings_init(void);

/* ESP_ERR_NVS_NOT_FOUND when the key (or the namespace) does not exist. */
esp_err_t settings_get_str(const char *ns, const char *key, char *buf, size_t len);
esp_err_t settings_set_str(const char *ns, const char *key, const char *val);
esp_err_t settings_get_u32(const char *ns, const char *key, uint32_t *val);
esp_err_t settings_set_u32(const char *ns, const char *key, uint32_t val);
esp_err_t settings_get_blob(const char *ns, const char *key, void *buf, size_t *len);
esp_err_t settings_set_blob(const char *ns, const char *key, const void *val, size_t len);

/* A missing key is not an error. */
esp_err_t settings_erase(const char *ns, const char *key);

/* Several strings of one namespace with one open and one commit; val NULL
 * erases that key. Stops at the first failure. Not atomic: a reset in the
 * middle can leave some keys written. */
typedef struct {
    const char *key;
    const char *val;
} settings_kv_t;
esp_err_t settings_set_many(const char *ns, const settings_kv_t *kv, int n);
