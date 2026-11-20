/* The one task that acts on link state (wiki Mower-Gateway-Architecture §4.1):
 * confirms a new image once MQTT is ready, publishes gateway/state when the
 * server link comes up (and once a second when a field changed), sends the
 * final ack of an update that restarted the board, and logs a 60 s alive line. */
#pragma once

#include "esp_err.h"

/* After gw_link_init(), remote_cmd_init() and ota_boot_init(). */
esp_err_t supervisor_start(void);
