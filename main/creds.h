#pragma once

/* The device identity is the client certificate in NVS (main/devcert.c, written by
 * tools/provision_cert.ps1). There are no passwords on the board. */
#define CREDS_DEVICE_MAX 32   /* letters, digits, '-', '_' */

/* The device name: MQTT username and client id, and the <name> in mower/<name>/...
 * The CN of the stored client certificate; "unprovisioned" while the board has none (it does
 * not connect then). Call devcert_init() first. */
const char *creds_device_name(void);
