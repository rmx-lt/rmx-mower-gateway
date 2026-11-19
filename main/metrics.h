/* The gateway's metric catalogue and telemetry (wiki Mower-Protocol §6.3, §16),
 * and the task that publishes the catalogue when the server link comes up and
 * drives telemetry and the monitor streams (monitor.h). Modules register their
 * own metrics. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* One catalogue entry (§16.1). part is "signal", "usage" or "system". */
typedef struct {
    const char *name;
    const char *key;
    const char *label;
    const char *description;
    const char *unit;
    const char *part;
    bool counter;
    int period;       /* seconds in telemetry */
    bool monitor;     /* offers the monitor rates */
    bool chart;
    bool has_min, has_max, has_low, has_high;
    double min, max, low, high;
    int decimals;
} metric_def_t;

/* Reads the value; false when it is not known (null). Runs on the publisher
 * task: must be quick. */
typedef bool (*metric_read_fn)(void *ctx, double *out);

/* Adds a metric to the catalogue. def must stay valid (static). Before the
 * first catalogue publish (bump CATALOGUE_REVISION in metrics.c when the set
 * changes); the same name again is ignored, so a module may register on every
 * (re)start. ESP_ERR_INVALID_STATE after the catalogue went out, ESP_ERR_NO_MEM
 * when the table is full. */
esp_err_t metrics_register(const metric_def_t *def, metric_read_fn fn, void *ctx);

/* Registers the system metrics and starts the publisher task (100 ms tick:
 * catalogue, telemetry, monitor streams) and the task that refreshes the signal
 * reading. Call after the modules registered theirs, before the server link is
 * up. */
void metrics_start(void);

/* For monitor.c: a monitorable metric, by name (family members must exist). */
typedef struct {
    int8_t type;     /* 0 registered metric, 1 up family member, 2 down family member */
    int16_t idx;
} metric_ref_t;

bool metrics_resolve(const char *name, metric_ref_t *out);
bool metrics_read(metric_ref_t ref, double *out);
/* A rate (samples per second) every monitorable metric offers. */
bool metrics_rate_offered(int rate);
