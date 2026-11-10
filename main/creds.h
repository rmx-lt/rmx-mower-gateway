#pragma once

#include <stddef.h>

#include "esp_err.h"

/* Namespace "creds" in NVS: the device key and certificate chain (main/devcert.c, written by
 * tools/provision_cert.ps1) and, until the certificate login is live, the device name and the MQTT
 * password (tools/provision_nvs.ps1). Secrets are never part of the firmware image. */
#define CREDS_DEVICE  "device"
#define CREDS_MQTT_PW "mqtt_pw"
#define CREDS_MAX_LEN 128
#define CREDS_DEVICE_MAX 32   /* letters, digits, '-', '_' */

/* Copies the string stored under key into buf. ESP_ERR_NOT_FOUND if it is missing,
 * ESP_ERR_INVALID_SIZE if buf is too small. Logs the key name, never the value. */
esp_err_t creds_get(const char *key, char *buf, size_t len);

/* The device name: MQTT username and client id, and the <name> in mower/<name>/...
 * The CN of the client certificate when there is one; else NVS (CREDS_DEVICE); else
 * CONFIG_PROBE_DEVICE_NAME. Call devcert_init() first, then this from app_main before other
 * tasks start. */
const char *creds_device_name(void);
