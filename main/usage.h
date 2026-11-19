/* Data usage counters (wiki Mower-Protocol §6.3): bytes on the PPP interface and
 * whole MQTT packets per topic. Counters only grow; they start at 0 at boot. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_netif.h"

#define USAGE_MAX_TOPICS 32

/* Call when PPP has an IP (every time: a redial makes a new lwIP netif). Counts
 * the IP packets of that interface in both directions from then on. The first
 * call registers usage.total.up and usage.total.down. */
void usage_attach_netif(esp_netif_t *ppp);

uint64_t usage_total_up(void);     /* mower -> network, bytes */
uint64_t usage_total_down(void);   /* network -> mower, bytes */

/* The device's topic prefix ("mower/<name>/"); it is cut off the names of the
 * per-topic counters. */
void usage_set_prefix(const char *prefix);

/* Counts one MQTT PUBLISH packet (fixed header, topic, packet id, payload). */
void usage_mqtt_count(bool up, const char *topic, size_t topic_len, size_t payload_len, int qos);

/* Per-topic counters, in the order the topics first carried data. A topic index
 * stays valid; names never change. */
int usage_topic_count(void);
const char *usage_topic_name(int idx);
uint64_t usage_topic_up(int idx);
uint64_t usage_topic_down(int idx);
