/* Firmware updates over MQTT (wiki Mower-Protocol §6.5, §12) and the first-boot
 * check of a new image. */
#pragma once

#include <stdbool.h>

/* Call once at boot. Logs the running slot. If this image is new (pending
 * verify), arms a timer that rolls back to the previous image unless
 * ota_confirm() is called within CONFIG_GATEWAY_OTA_CONFIRM_S. */
void ota_boot_init(void);

/* Call after remote_cmd_init(): an ota command that was open when the board
 * restarted is remembered, so a resend of its id gets "accepted" again. */
void ota_resume_pending(void);

/* The new image works (MQTT connected): cancel the rollback. Safe to call
 * any number of times. */
void ota_confirm(void);

/* After a restart that an ota command caused: sends its final ack once the image
 * is confirmed and MQTT is up: "done" with {"version","previous","partition"}
 * when the new image runs, "failed" with error rolled_back when the old one
 * runs again. Returns true when nothing is left to report (no update was open,
 * or the ack went out), false when it has to be called again later. Safe to call
 * often. */
bool ota_report_result(void);

/* True while a download runs (and until the restart). */
bool ota_busy(void);

/* Checks the request and starts the download on its own task. file is the
 * image name on the server (<version>.bin). On success the "accepted" ack has
 * been sent, the task downloads over gateway/fw/..., checks and writes the
 * image, stores the command id in NVS and restarts. A failure ends with the
 * final ack "failed" (download_failed, bad_image, bad_size, same_version).
 * Returns false with an error code and message when the request is refused:
 * the caller sends the final ack. */
bool ota_start(const char *cmd_id, const char *file, const char *version, int size,
               const char **error, const char **message);
