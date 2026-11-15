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

/* Publishes gateway/state (retained, QoS 1) when it differs from the last
 * published one, or always with force. */
void gw_state_publish(bool force);
