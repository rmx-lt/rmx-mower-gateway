#include "devcert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bootloader_random.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "creds.h"

static const char *TAG = "devcert";

#define NVS_NS     "creds"
#define NVS_KEY    "key"
#define NVS_CHAIN  "chain"
#define KEY_MAX    512    /* SEC1 PEM of a P-256 key is about 230 bytes */
#define CHAIN_MAX  3072   /* device + issuing certificate, about 1.4 KB; NVS strings may reach 4000 */
#define CSR_MAX    1024

static char s_key[KEY_MAX];
static char *s_chain;          /* heap, only when a matching certificate is stored */
static char s_cn[CREDS_DEVICE_MAX + 1];
static bool s_have_key;

static bool valid_cn(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > CREDS_DEVICE_MAX) {
        return false;
    }
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

static esp_err_t nvs_read(const char *key, char *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t n = len;
        err = nvs_get_str(h, key, buf, &n);
        nvs_close(h);
    }
    return err;
}

static esp_err_t nvs_write(const char *key, const char *val)   /* val NULL = erase */
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = val ? nvs_set_str(h, key, val) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND && !val) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t generate_key(void)
{
    ESP_LOGI(TAG, "no key in NVS: generating an ECDSA P-256 key");
    /* The ESP32 RNG is only a true RNG while the RF noise source or the SAR ADC feeds it. Neither
     * Wi-Fi nor Bluetooth runs here, so turn the ADC noise source on for the key generation. */
    bootloader_random_enable();
    psa_status_t st = psa_crypto_init();
    psa_key_id_t id = 0;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    esp_err_t err = ESP_FAIL;
    if (st == PSA_SUCCESS) {
        psa_key_attributes_t at = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&at, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&at, 256);
        psa_set_key_usage_flags(&at, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH |
                                         PSA_KEY_USAGE_SIGN_MESSAGE);
        psa_set_key_algorithm(&at, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
        st = psa_generate_key(&at, &id);
        psa_reset_key_attributes(&at);
    }
    bootloader_random_disable();
    if (st == PSA_SUCCESS) {
        int rc = mbedtls_pk_copy_from_psa(id, &pk);
        psa_destroy_key(id);
        if (rc == 0 && mbedtls_pk_write_key_pem(&pk, (unsigned char *)s_key, sizeof(s_key)) == 0) {
            err = nvs_write(NVS_KEY, s_key);
        } else {
            ESP_LOGE(TAG, "exporting the key failed (%d)", rc);
        }
    } else {
        ESP_LOGE(TAG, "psa_generate_key failed (%d)", (int)st);
    }
    mbedtls_pk_free(&pk);
    if (err != ESP_OK) {
        memset(s_key, 0, sizeof(s_key));
    }
    return err;
}

/* Checks a PEM chain against the key. Returns NULL and fills cn (CREDS_DEVICE_MAX+1 bytes) when
 * it is fine, else the reason. */
static const char *check_chain(const char *pem, char *cn)
{
    mbedtls_x509_crt crt;
    mbedtls_pk_context key;
    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_init(&key);
    const char *why = NULL;
    char dn[96];
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, strlen(pem) + 1) != 0) {
        why = "unparsable chain";
    } else if (mbedtls_x509_dn_gets(dn, sizeof(dn), &crt.subject) < 0 ||
               strncmp(dn, "CN=", 3) != 0 || !valid_cn(dn + 3)) {
        why = "subject is not a single valid CN";
    } else if (mbedtls_pk_parse_key(&key, (const unsigned char *)s_key, strlen(s_key) + 1, NULL, 0) != 0) {
        why = "key unreadable";
    } else if (mbedtls_pk_check_pair(&crt.pk, &key) != 0) {
        why = "certificate does not match the key";
    } else {
        memcpy(cn, dn + 3, strlen(dn + 3) + 1);   /* valid_cn bounded it */
    }
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&crt);
    return why;
}

esp_err_t devcert_init(void)
{
    esp_err_t err = nvs_read(NVS_KEY, s_key, sizeof(s_key));
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = generate_key();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no usable key (%s): password login only", esp_err_to_name(err));
        return err;
    }
    s_have_key = true;

    char *chain = malloc(CHAIN_MAX);
    if (!chain) {
        return ESP_ERR_NO_MEM;
    }
    err = nvs_read(NVS_CHAIN, chain, CHAIN_MAX);
    if (err == ESP_OK) {
        char cn[CREDS_DEVICE_MAX + 1];
        const char *why = check_chain(chain, cn);
        if (!why) {
            snprintf(s_cn, sizeof(s_cn), "%s", cn);
            s_chain = chain;
            ESP_LOGI(TAG, "client certificate for %s", s_cn);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "stored certificate rejected (%s): password login only", why);
    } else {
        ESP_LOGI(TAG, "no client certificate in NVS");
    }
    free(chain);
    return ESP_OK;
}

const char *devcert_cn(void) { return s_chain ? s_cn : NULL; }
const char *devcert_chain_pem(void) { return s_chain; }
const char *devcert_key_pem(void) { return s_chain ? s_key : NULL; }

void devcert_print_csr(const char *cn)
{
    if (!s_have_key) {
        return;
    }
    mbedtls_pk_context key;
    mbedtls_x509write_csr csr;
    mbedtls_pk_init(&key);
    mbedtls_x509write_csr_init(&csr);
    unsigned char *pem = malloc(CSR_MAX);
    char subject[CREDS_DEVICE_MAX + 8];
    snprintf(subject, sizeof(subject), "CN=%s", cn);
    int rc = pem ? mbedtls_pk_parse_key(&key, (const unsigned char *)s_key, strlen(s_key) + 1, NULL, 0) : -1;
    if (rc == 0) {
        mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
        mbedtls_x509write_csr_set_key(&csr, &key);
        rc = mbedtls_x509write_csr_set_subject_name(&csr, subject);
    }
    if (rc == 0) {
        rc = mbedtls_x509write_csr_pem(&csr, pem, CSR_MAX);
    }
    if (rc == 0) {
        printf("\n%s\n%s%s\n", DEVCERT_CSR_BEGIN, (char *)pem, DEVCERT_CSR_END);
        fflush(stdout);
    } else {
        ESP_LOGE(TAG, "building the CSR failed (%d)", rc);
    }
    free(pem);
    mbedtls_x509write_csr_free(&csr);
    mbedtls_pk_free(&key);
}

/* ---- serial command task ---- */

static void install_chain(const char *pem)
{
    char cn[CREDS_DEVICE_MAX + 1];
    const char *why = check_chain(pem, cn);
    if (!why) {
        esp_err_t err = nvs_write(NVS_CHAIN, pem);
        if (err != ESP_OK) {
            printf("RMX-CERT-ERR nvs %s\n", esp_err_to_name(err));
            return;
        }
        printf("RMX-CERT-OK %s\n", cn);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    printf("RMX-CERT-ERR %s\n", why);
}

static void console_task(void *arg)
{
    char line[160];
    size_t n = 0;
    char *cap = NULL;       /* chain being received */
    size_t cap_len = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c != '\n') {
            if (c != '\r' && n < sizeof(line) - 1) {
                line[n++] = (char)c;
            }
            continue;
        }
        line[n] = '\0';
        n = 0;
        if (strcmp(line, "RMX-CERT-BEGIN") == 0) {
            free(cap);
            cap = calloc(1, CHAIN_MAX + 1);
            cap_len = 0;
        } else if (strcmp(line, "RMX-CERT-END") == 0) {
            if (cap) {
                if (cap_len == 0 || cap_len >= CHAIN_MAX) {
                    printf("RMX-CERT-ERR chain size\n");
                } else {
                    install_chain(cap);
                }
                free(cap);
                cap = NULL;
            }
        } else if (cap) {
            size_t l = strlen(line);
            if (cap_len + l + 1 >= CHAIN_MAX) {
                cap_len = CHAIN_MAX;   /* too long: refused at END */
            } else {
                memcpy(cap + cap_len, line, l);
                cap[cap_len + l] = '\n';
                cap_len += l + 1;
            }
        } else if (strcmp(line, "RMX-CSR") == 0) {
            devcert_print_csr(creds_device_name());
        } else if (strcmp(line, "RMX-CERT-CLEAR") == 0) {
            esp_err_t err = nvs_write(NVS_CHAIN, NULL);
            printf(err == ESP_OK ? "RMX-CERT-OK cleared\n" : "RMX-CERT-ERR nvs\n");
            fflush(stdout);
            if (err == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        }
    }
}

void devcert_console_start(void)
{
    if (!s_have_key) {
        return;
    }
    if (uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "UART0 driver install failed: no serial provisioning");
        return;
    }
    xTaskCreate(console_task, "devcert", 6144, NULL, 3, NULL);
}
