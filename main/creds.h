#pragma once

#include <stddef.h>

#include "esp_err.h"

/* Device passwords live in NVS (namespace "creds"), written by tools/provision_nvs.ps1.
 * They are never part of the firmware image. */
#define CREDS_MQTT_PW "mqtt_pw"
#define CREDS_WEB_PW  "web_pw"
#define CREDS_OTA_PW  "ota_pw"
#define CREDS_MAX_LEN 128

/* Copies the string stored under key into buf. ESP_ERR_NOT_FOUND if it is missing,
 * ESP_ERR_INVALID_SIZE if buf is too small. Logs the key name, never the value. */
esp_err_t creds_get(const char *key, char *buf, size_t len);
