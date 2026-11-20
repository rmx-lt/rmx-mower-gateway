/* Gateway identity facts of the contract (wiki Mower-Protocol §4.1, §6.2): boot
 * counter, clock, the retained gateway/state message, and the small NVS
 * namespace "gw" that holds what has to survive a restart. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#define GW_PROTOCOL "0.1"

/* After nvs_flash_init(): counts this boot (kept in NVS). */
void gw_boot_init(void);
uint32_t gw_boot(void);

/* The SNTP clock is set. */
bool gw_clock_valid(void);
/* Unix time with fraction, or -1 while the clock is not set. */
double gw_time(void);
/* Adds "time": a number, or null while the clock is not set. */
void gw_add_time(cJSON *obj, bool fraction);

/* Small strings in NVS namespace "gw" (keys up to 15 chars). */
bool gw_nvs_get(const char *key, char *buf, size_t len);
bool gw_nvs_set(const char *key, const char *val);
void gw_nvs_erase(const char *key);

/* Sets a module-owned field of gateway/state (§6.2), e.g. "operator" (cell),
 * "autopilot_link", "ntrip"; value NULL is JSON null. Copied; callable from any
 * task, also before gw_boot_init(). Fields keep the order they were first set
 * in; autopilot_link ("down"), ntrip ("off") and operator (null) exist from the
 * start. Up to 8 fields of up to 31 chars. */
void gw_state_set(const char *field, const char *value);

/* Publishes gateway/state (retained, QoS 1) when it differs from the last
 * published one, or always with force. */
void gw_state_publish(bool force);
