/* Per-device client certificate (wiki Mower-Gateway-Server-Contract "Client certificate").
 *
 * The ECDSA P-256 key is generated on the board at first boot and stored in NVS (namespace
 * "creds", key "key"). It is never printed or sent anywhere. While no certificate is stored the
 * board prints a CSR on the serial log between marker lines; tools/provision_cert.ps1 has it
 * signed and sends the chain back over the same port. The chain (device cert + issuing CA cert,
 * PEM) is stored under "chain" without touching the key or any other NVS key.
 *
 * Serial commands (UART0, one line each, 115200 8N1):
 *   RMX-CSR                           print the CSR again
 *   RMX-CERT-BEGIN ... RMX-CERT-END   PEM chain in between; checked, stored, then a restart
 *   RMX-CERT-CLEAR                    delete the chain (back to the password login), then a restart
 * Answers: RMX-CERT-OK <cn> or RMX-CERT-ERR <reason>. */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define DEVCERT_CSR_BEGIN "=====RMX-CSR-BEGIN====="
#define DEVCERT_CSR_END   "=====RMX-CSR-END====="

/* Loads the key (generating and storing it on first boot) and the chain. Call after
 * nvs_flash_init, first thing in app_main. A broken chain is ignored (password fallback). */
esp_err_t devcert_init(void);

/* Subject CN of the stored certificate, NULL when there is none (or it does not match the key). */
const char *devcert_cn(void);

/* PEM strings for esp-mqtt (valid for the life of the program); NULL without a certificate. */
const char *devcert_chain_pem(void);
const char *devcert_key_pem(void);

/* Prints the CSR (subject CN = cn) between the marker lines. */
void devcert_print_csr(const char *cn);

/* Starts the serial command task. Call once, after devcert_init. */
void devcert_console_start(void);
