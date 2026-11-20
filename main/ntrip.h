/* NTRIP client (wiki Mower-Protocol §13): caster settings in NVS (commands
 * ntrip_set / ntrip_clear), its own TCP connection over PPP, the autopilot's
 * GGA to the caster every 10 s, the caster's bytes to a corrections sink.
 * Reports "ntrip" in gateway/state, GW_NTRIP_STREAM (heartbeat bit 2) and the
 * metrics usage.ntrip.up|down. The password is never logged or reported. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Registers the commands and metrics, loads the settings and starts the task.
 * Before mqtt_link_start() and metrics_start(). */
void ntrip_start(void);

/* A GGA sentence from the autopilot (board link). Callable from any task;
 * copies, never blocks for long. */
void ntrip_feed_gga(const char *nmea, size_t len);

/* Where the caster's bytes go (the board link's CORRECTIONS frames, #62).
 * Called on the ntrip task; must not block. Without a sink they are counted
 * and dropped. */
typedef void (*ntrip_sink_fn)(const uint8_t *data, size_t len);
void ntrip_set_sink(ntrip_sink_fn fn);
