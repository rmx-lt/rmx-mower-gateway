#pragma once

#include <stddef.h>

#include "esp_err.h"

/* Device name, passwords and the HTTPS user live in NVS (namespace "creds"), written by
 * tools/provision_nvs.ps1. Passwords are never part of the firmware image. */
#define CREDS_DEVICE  "device"
#define CREDS_MQTT_PW "mqtt_pw"
#define CREDS_HTTPS_USER "https_user"
#define CREDS_WEB_PW  "web_pw"
#define CREDS_OTA_PW  "ota_pw"
#define CREDS_MAX_LEN 128
#define CREDS_DEVICE_MAX 32   /* letters, digits, '-', '_' */

/* Copies the string stored under key into buf. ESP_ERR_NOT_FOUND if it is missing,
 * ESP_ERR_INVALID_SIZE if buf is too small. Logs the key name, never the value. */
esp_err_t creds_get(const char *key, char *buf, size_t len);

/* The device name: MQTT username and client id, and the <name> in mower/<name>/...
 * Read from NVS (CREDS_DEVICE) on the first call; CONFIG_PROBE_DEVICE_NAME when it is
 * missing or invalid. Call it first from app_main, before other tasks start. */
const char *creds_device_name(void);

/* The login for the legacy HTTPS API and the firmware download (web_pw, ota_pw): the
 * NVS key CREDS_HTTPS_USER, or "probe-device" when it is missing or invalid. It is
 * independent of the device name (the MQTT login mower-01 has no web user). */
const char *creds_https_user(void);
