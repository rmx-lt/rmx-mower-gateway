#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "cell.h"
#include "mqtt_link.h"
#include "esp_random.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

#define REQUEST_ID_LEN 32
/* Project name of images built before the rename; one of them may update to this project. */
#define LEGACY_PROJECT_NAME "rmx_mower_modem_probe"

/* A unique request id for one firmware request ("dev-<boot>-<n>"). */
static void new_request_id(char buf[REQUEST_ID_LEN + 1])
{
    static uint32_t boot_tag;
    static uint32_t counter;
    if (!boot_tag) {
        boot_tag = esp_random() | 1;
    }
    snprintf(buf, REQUEST_ID_LEN + 1, "dev-%08lx-%lu", (unsigned long)boot_tag, (unsigned long)++counter);
}

/* wiki Mower-Gateway-Server-Contract, "Firmware over MQTT": the frame is the
 * contract in rmx-server-app (Server-App, "Firmware over MQTT"). One request
 * is in flight. A dropped link retries the same chunk; bytes already written
 * with esp_ota_write stay, so the next try resumes at the next index. */
#define CHUNK_TRIES     4
#define CHUNK_WAIT_MS   45000
#define ID_LEN          64
#define FILE_LEN        128
#define VERSION_LEN     32
#define FW_HDR_LEN      64
#define FW_MAX_FRAME    (FW_HDR_LEN + 16384)

#define APP_DESC_BYTES  (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t version;
    uint8_t flags;
    uint16_t header_len;
    uint32_t chunk_index;
    uint32_t chunk_count;
    uint32_t image_size;
    uint16_t chunk_len;
    uint16_t reserved;
    uint8_t request_id_hash[8];
    uint8_t sha256[32];
} rmx_fw_hdr_t;

_Static_assert(sizeof(rmx_fw_hdr_t) == FW_HDR_LEN, "firmware chunk header is 64 bytes");

typedef struct {
    char id[ID_LEN + 1];
    char file[FILE_LEN + 1];
    char version[VERSION_LEN];
    int size;                   /* expected image size, 0 = don't check */
} ota_req_t;

typedef struct {
    uint8_t *data;
    size_t len;
    bool chunk;                 /* false: JSON on fw/res */
} fw_rx_t;

static ota_req_t s_req;
static volatile bool s_busy;
static volatile bool s_listen;
static QueueHandle_t s_rxq;
static esp_timer_handle_t s_confirm_timer;
static bool s_pending_verify;
static bool s_report_pending;

static const char *volatile s_state = "idle";
static char s_target[VERSION_LEN];
static volatile int s_progress = -1;
static char s_error[32];

static bool clock_valid(void)
{
    return time(NULL) > 1700000000;
}

static void send_ack(const char *state, const char *error, const char *message, int64_t t0)
{
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "id", s_req.id);
    cJSON_AddStringToObject(reply, "cmd", "ota");
    if (error) {
        cJSON_AddBoolToObject(reply, "ok", false);
        cJSON_AddStringToObject(reply, "error", error);
        cJSON_AddStringToObject(reply, "message", message);
    } else {
        cJSON_AddBoolToObject(reply, "ok", true);
        cJSON *res = cJSON_AddObjectToObject(reply, "result");
        cJSON_AddStringToObject(res, "state", state);
        cJSON_AddStringToObject(res, "version", s_req.version);
        cJSON_AddStringToObject(res, "from", esp_app_get_description()->version);
    }
    cJSON_AddNumberToObject(reply, "ms", (double)((esp_timer_get_time() - t0) / 1000));
    if (clock_valid()) {
        cJSON_AddNumberToObject(reply, "time", (double)time(NULL));
    }
    char *json = cJSON_PrintUnformatted(reply);
    if (json) {
        mqtt_link_send_ack(json);
        free(json);
    }
    cJSON_Delete(reply);
}

static void fail(const char *error, const char *message, int64_t t0)
{
    ESP_LOGE(TAG, "update to %s failed: %s (%s)", s_req.version, error, message);
    snprintf(s_error, sizeof(s_error), "%s", error);
    s_state = "failed";
    send_ack(NULL, error, message, t0);
}

static void on_fw(const uint8_t *data, size_t len, bool chunk)
{
    if (!s_listen || !s_rxq || len == 0 || len > FW_MAX_FRAME) {
        return;
    }
    uint8_t *copy = malloc(len);
    if (!copy) {
        ESP_LOGE(TAG, "no memory for a %u-byte firmware message", (unsigned)len);
        return;
    }
    memcpy(copy, data, len);
    fw_rx_t m = { .data = copy, .len = len, .chunk = chunk };
    if (xQueueSend(s_rxq, &m, 0) != pdTRUE) {
        fw_rx_t old;
        if (xQueueReceive(s_rxq, &old, 0) == pdTRUE) {
            free(old.data);
        }
        if (xQueueSend(s_rxq, &m, 0) != pdTRUE) {
            free(copy);
        }
    }
}

static void drain_rx(void)
{
    fw_rx_t m;
    while (s_rxq && xQueueReceive(s_rxq, &m, 0) == pdTRUE) {
        free(m.data);
    }
}

static void finish_task(void) __attribute__((noreturn));

static void finish_task(void)
{
    s_listen = false;
    mqtt_link_set_fw_handler(NULL);
    drain_rx();
    s_busy = false;
    vTaskDelete(NULL);
    abort();
}

static void restart_into_new_image(void)
{
    ESP_LOGW(TAG, "restarting into %s in 3 s", s_req.version);
    vTaskDelay(pdMS_TO_TICKS(3000));
    mqtt_link_stop();
    cell_stop();
    esp_restart();
}

static bool sha256_id(const char *rid, uint8_t out[8])
{
    uint8_t full[32];
    size_t n = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)rid, strlen(rid), full, sizeof(full), &n) != PSA_SUCCESS ||
            n < 8) {
        return false;
    }
    memcpy(out, full, 8);
    return true;
}

/* False when the frame is not ours or not a version-1 header. Payload points
 * into data. */
static bool parse_frame(const uint8_t *data, size_t len, const uint8_t expect_hash[8],
                        rmx_fw_hdr_t *hdr, const uint8_t **payload)
{
    if (len < FW_HDR_LEN) {
        return false;
    }
    memcpy(hdr, data, FW_HDR_LEN);
    if (memcmp(hdr->magic, "RMFW", 4) != 0 || hdr->version != 1 || hdr->flags != 0x01 ||
            hdr->header_len != FW_HDR_LEN || hdr->reserved != 0) {
        return false;
    }
    if (len != FW_HDR_LEN + (size_t)hdr->chunk_len) {
        return false;
    }
    if (memcmp(hdr->request_id_hash, expect_hash, 8) != 0) {
        return false;
    }
    *payload = data + FW_HDR_LEN;
    return true;
}

static bool parse_fw_error(const uint8_t *data, size_t len, const char *rid,
                           char *code, size_t code_len, char *msg, size_t msg_len)
{
    cJSON *j = cJSON_ParseWithLength((const char *)data, len);
    if (!j) {
        return false;
    }
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(j, "request_id"));
    cJSON *err = cJSON_GetObjectItem(j, "error");
    bool match = id && strcmp(id, rid) == 0 && cJSON_IsObject(err);
    if (match) {
        const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(err, "code"));
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message"));
        snprintf(code, code_len, "%s", (c && c[0]) ? c : "failed");
        snprintf(msg, msg_len, "%s", (m && m[0]) ? m : "firmware request failed");
    }
    cJSON_Delete(j);
    return match;
}

typedef enum {
    RX_CHUNK,
    RX_TIMEOUT,
    RX_SERVER,
} rx_kind_t;

/* Waits for the chunk with this request id, or a JSON error for it.
 * On RX_CHUNK the caller frees *frame. */
static rx_kind_t wait_chunk(const char *rid, const uint8_t expect_hash[8], int timeout_ms,
                            uint8_t **frame, size_t *frame_len,
                            char *err_code, size_t err_code_len, char *err_msg, size_t err_msg_len)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) {
            return RX_TIMEOUT;
        }
        fw_rx_t m;
        if (xQueueReceive(s_rxq, &m, pdMS_TO_TICKS((left_us + 999) / 1000)) != pdTRUE) {
            return RX_TIMEOUT;
        }
        if (!m.chunk) {
            bool server = parse_fw_error(m.data, m.len, rid, err_code, err_code_len, err_msg, err_msg_len);
            free(m.data);
            if (server) {
                return RX_SERVER;
            }
            continue;
        }
        rmx_fw_hdr_t hdr;
        const uint8_t *payload = NULL;
        bool ours = parse_frame(m.data, m.len, expect_hash, &hdr, &payload);
        if (!ours) {
            free(m.data);
            continue;
        }
        *frame = m.data;
        *frame_len = m.len;
        return RX_CHUNK;
    }
}

static bool link_up(int attempt)
{
    if (mqtt_link_wait_ready(0)) {
        return true;
    }
    ESP_LOGW(TAG, "MQTT down before chunk retry %d", attempt);
    return cell_wait_ip(120000) && mqtt_link_wait_ready(90000);
}

static bool fetch_chunk(uint32_t index, uint8_t **frame, size_t *frame_len, int64_t t0)
{
    char rid[REQUEST_ID_LEN + 1];
    new_request_id(rid);
    uint8_t expect[8];
    if (!sha256_id(rid, expect)) {
        fail("failed", "could not hash the firmware request id", t0);
        return false;
    }
    char body[FILE_LEN + 96];
    snprintf(body, sizeof(body),
             "{\"request_id\":\"%s\",\"op\":\"chunk\",\"file\":\"%s\",\"chunk\":%lu}",
             rid, s_req.file, (unsigned long)index);

    for (int attempt = 0; attempt < CHUNK_TRIES; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "chunk %lu retry %d/%d", (unsigned long)index, attempt, CHUNK_TRIES - 1);
            vTaskDelay(pdMS_TO_TICKS(2000 * attempt));
        }
        if (!link_up(attempt)) {
            continue;
        }
        if (mqtt_link_publish_fw_req(body) != ESP_OK) {
            continue;
        }
        char code[24];
        char message[96];
        rx_kind_t kind = wait_chunk(rid, expect, CHUNK_WAIT_MS, frame, frame_len,
                                    code, sizeof(code), message, sizeof(message));
        if (kind == RX_CHUNK) {
            return true;
        }
        if (kind == RX_SERVER) {
            /* Stable machine code from the server. "firmware is not configured"
             * means FIRMWARE_DIR is unset: retrying will not help. */
            const char *use = code;
            if (strcmp(code, "bad_request") != 0 && strcmp(code, "not_found") != 0 &&
                    strcmp(code, "internal") != 0) {
                use = "failed";
            }
            fail(use, message, t0);
            return false;
        }
    }
    fail("download_failed", "no firmware chunk (the link dropped or firmware is not served)", t0);
    return false;
}

static bool check_desc(const esp_partition_t *part, int64_t t0)
{
    esp_app_desc_t desc;
    const esp_app_desc_t *running = esp_app_get_description();
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) {
        fail("bad_image", "could not read the image header", t0);
        return false;
    }
    ESP_LOGI(TAG, "image: %s %s (IDF %s)", desc.project_name, desc.version, desc.idf_ver);
    bool renamed = strncmp(running->project_name, LEGACY_PROJECT_NAME, sizeof(desc.project_name)) == 0 &&
                   strncmp(desc.project_name, "rmx_mower_gateway", sizeof(desc.project_name)) == 0;
    if (!renamed && strncmp(desc.project_name, running->project_name, sizeof(desc.project_name)) != 0) {
        fail("bad_image", "the image is for another project", t0);
        return false;
    }
    if (strncmp(desc.version, s_req.version, sizeof(desc.version)) != 0) {
        fail("bad_image", "the image has another version than the command", t0);
        return false;
    }
    if (strncmp(desc.version, running->version, sizeof(desc.version)) == 0) {
        fail("same_version", "this version is already running", t0);
        return false;
    }
    return true;
}

static void ota_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    s_listen = true;
    mqtt_link_set_fw_handler(on_fw);
    drain_rx();
    if (psa_crypto_init() != PSA_SUCCESS) {
        fail("failed", "crypto init failed", t0);
        finish_task();
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        fail("failed", "no OTA partition", t0);
        finish_task();
    }

    esp_ota_handle_t handle = 0;
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    bool sha_open = false;
    uint32_t image_size = 0;
    uint32_t chunk_count = 0;
    uint8_t expect_sha[32];
    bool have_meta = false;
    bool desc_checked = false;
    size_t written = 0;
    int last_logged = -1;

    for (uint32_t index = 0; !have_meta || index < chunk_count; index++) {
        uint8_t *frame = NULL;
        size_t frame_len = 0;
        if (!fetch_chunk(index, &frame, &frame_len, t0)) {
            if (handle) {
                esp_ota_abort(handle);
            }
            if (sha_open) {
                psa_hash_abort(&sha);
            }
            finish_task();
        }
        rmx_fw_hdr_t hdr;
        memcpy(&hdr, frame, FW_HDR_LEN);
        const uint8_t *payload = frame + FW_HDR_LEN;

        if (hdr.chunk_index != index) {
            free(frame);
            fail("bad_image", "firmware chunk index does not match the request", t0);
            if (handle) {
                esp_ota_abort(handle);
            }
            if (sha_open) {
                psa_hash_abort(&sha);
            }
            finish_task();
        }
        if (!have_meta) {
            image_size = hdr.image_size;
            chunk_count = hdr.chunk_count;
            memcpy(expect_sha, hdr.sha256, sizeof(expect_sha));
            if (image_size == 0 || image_size > part->size || chunk_count == 0 ||
                    chunk_count > (part->size / 256) + 1) {
                free(frame);
                fail("bad_size", "firmware image size is not usable", t0);
                finish_task();
            }
            if (s_req.size > 0 && (uint32_t)s_req.size != image_size) {
                free(frame);
                ESP_LOGE(TAG, "image size %lu, expected %d", (unsigned long)image_size, s_req.size);
                fail("bad_size", "the image size differs from the command", t0);
                finish_task();
            }
            if (psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
                free(frame);
                fail("failed", "could not start the image hash", t0);
                finish_task();
            }
            sha_open = true;
            if (esp_ota_begin(part, image_size, &handle) != ESP_OK) {
                free(frame);
                psa_hash_abort(&sha);
                fail("failed", "could not start writing the update slot", t0);
                finish_task();
            }
            have_meta = true;
        } else if (hdr.image_size != image_size || hdr.chunk_count != chunk_count ||
                   memcmp(hdr.sha256, expect_sha, sizeof(expect_sha)) != 0) {
            free(frame);
            esp_ota_abort(handle);
            psa_hash_abort(&sha);
            fail("bad_image", "firmware chunk header changed mid-download", t0);
            finish_task();
        }

        if (hdr.chunk_len > 0) {
            if (esp_ota_write(handle, payload, hdr.chunk_len) != ESP_OK ||
                    psa_hash_update(&sha, payload, hdr.chunk_len) != PSA_SUCCESS) {
                free(frame);
                esp_ota_abort(handle);
                psa_hash_abort(&sha);
                fail("failed", "writing the update slot failed", t0);
                finish_task();
            }
        }
        written += hdr.chunk_len;
        free(frame);

        if (!desc_checked && written >= APP_DESC_BYTES) {
            if (!check_desc(part, t0)) {
                esp_ota_abort(handle);
                psa_hash_abort(&sha);
                finish_task();
            }
            desc_checked = true;
        }
        s_progress = (int)((uint64_t)written * 100 / image_size);
        if (s_progress / 10 != last_logged) {
            last_logged = s_progress / 10;
            ESP_LOGI(TAG, "%d %% (%u of %lu bytes, chunk %lu/%lu)", s_progress,
                     (unsigned)written, (unsigned long)image_size,
                     (unsigned long)index + 1, (unsigned long)chunk_count);
        }
    }

    if (!desc_checked) {
        esp_ota_abort(handle);
        psa_hash_abort(&sha);
        fail("bad_image", "image is too small to hold an app header", t0);
        finish_task();
    }
    if (written != image_size) {
        esp_ota_abort(handle);
        psa_hash_abort(&sha);
        fail("bad_size", "the chunks do not add up to the image size", t0);
        finish_task();
    }

    uint8_t got[32];
    size_t got_len = 0;
    psa_status_t st = psa_hash_finish(&sha, got, sizeof(got), &got_len);
    sha_open = false;
    if (st != PSA_SUCCESS || got_len != sizeof(expect_sha) || memcmp(got, expect_sha, sizeof(got)) != 0) {
        esp_ota_abort(handle);
        fail("bad_image", "image SHA-256 does not match the firmware header", t0);
        finish_task();
    }
    ESP_LOGI(TAG, "image SHA-256 matches the chunk header");

    /* esp_ota_end checks the image, including the RSA-3072 signature when
     * CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT is set. The handle is
     * released here, success or not. Boot slot changes only after that. */
    esp_err_t err = esp_ota_end(handle);
    handle = 0;
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        fail("bad_image", "image check failed (signature or hash, see the device log)", t0);
        finish_task();
    }
    if (err != ESP_OK) {
        fail("failed", esp_err_to_name(err), t0);
        finish_task();
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        fail("failed", esp_err_to_name(err), t0);
        finish_task();
    }

    ESP_LOGI(TAG, "update to %s written in %lld s", s_req.version, (esp_timer_get_time() - t0) / 1000000);
    s_progress = 100;
    s_state = "done";
    send_ack("done", NULL, NULL, t0);
    restart_into_new_image();
    finish_task();
}

static bool valid_fw_file(const char *name)
{
    size_t n = strlen(name);
    if (n < 5 || n > FILE_LEN || strcmp(name + n - 4, ".bin") != 0) {
        return false;
    }
    for (size_t i = 0; i < n - 4; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool ota_start(const char *cmd_id, const char *file, const char *version, int size,
               const char **error, const char **message)
{
    *error = "bad_request";
    if (!file || !valid_fw_file(file)) {
        *message = "ota needs a file name [A-Za-z0-9._-]+.bin (or a version, used as <version>.bin)";
        return false;
    }
    if (!version || !version[0] || strlen(version) >= VERSION_LEN) {
        *message = "ota needs a string version (1-31 chars)";
        return false;
    }
    if (size < 0) {
        *message = "size must be the image size in bytes";
        return false;
    }
    if (strcmp(version, esp_app_get_description()->version) == 0) {
        *error = "same_version";
        *message = "this version is already running";
        return false;
    }
    if (s_pending_verify) {
        *error = "busy";
        *message = "the running image is not confirmed yet";
        return false;
    }
    if (s_busy) {
        *error = "busy";
        *message = "an update is already running";
        return false;
    }
    int rssi = cell_last_rssi();
    if (rssi != 99 && rssi < CONFIG_GATEWAY_OTA_MIN_CSQ) {
        *error = "low_signal";
        *message = "signal too weak for an update (CSQ below CONFIG_GATEWAY_OTA_MIN_CSQ)";
        return false;
    }
    if (!s_rxq) {
        s_rxq = xQueueCreate(4, sizeof(fw_rx_t));
        if (!s_rxq) {
            *error = "failed";
            *message = "no memory for the update";
            return false;
        }
    }

    s_busy = true;
    snprintf(s_req.id, sizeof(s_req.id), "%s", cmd_id);
    snprintf(s_req.file, sizeof(s_req.file), "%s", file);
    snprintf(s_req.version, sizeof(s_req.version), "%s", version);
    s_req.size = size;
    snprintf(s_target, sizeof(s_target), "%s", version);
    s_error[0] = '\0';
    s_progress = 0;
    s_state = "downloading";
    if (xTaskCreate(ota_task, "ota", 12288, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
        s_state = "failed";
        *error = "failed";
        *message = "no memory for the update task";
        return false;
    }
    ESP_LOGI(TAG, "update %s -> %s file %s", esp_app_get_description()->version, version, file);
    return true;
}

bool ota_busy(void)
{
    return s_busy;
}

static void confirm_timeout(void *arg)
{
    ESP_LOGE(TAG, "new image not confirmed within %d s (no MQTT), rolling back",
             CONFIG_GATEWAY_OTA_CONFIRM_S);
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void ota_boot_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        state = ESP_OTA_IMG_UNDEFINED;
    }
    ESP_LOGI(TAG, "running from %s at 0x%lx, state %d", running->label,
             (unsigned long)running->address, (int)state);

    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    if (invalid && esp_ota_get_partition_description(invalid, &desc) == ESP_OK) {
        ESP_LOGW(TAG, "rolled back from %s (%s)", desc.version, invalid->label);
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        const esp_timer_create_args_t args = { .callback = confirm_timeout, .name = "ota_confirm" };
        if (esp_timer_create(&args, &s_confirm_timer) == ESP_OK) {
            esp_timer_start_once(s_confirm_timer, (uint64_t)CONFIG_GATEWAY_OTA_CONFIRM_S * 1000000);
        }
        ESP_LOGW(TAG, "new image: confirm within %d s or roll back", CONFIG_GATEWAY_OTA_CONFIRM_S);
    }
}

void ota_confirm(void)
{
    if (!s_pending_verify) {
        return;
    }
    if (s_confirm_timer) {
        esp_timer_stop(s_confirm_timer);
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_pending_verify = false;
        s_report_pending = true;
        ESP_LOGI(TAG, "image %s confirmed", esp_app_get_description()->version);
    }
}

void ota_add_status(cJSON *res)
{
    cJSON_AddStringToObject(res, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(res, "partition", esp_ota_get_running_partition()->label);
    cJSON_AddBoolToObject(res, "confirmed", !s_pending_verify);
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    if (invalid && esp_ota_get_partition_description(invalid, &desc) == ESP_OK) {
        cJSON_AddStringToObject(res, "rolled_back_from", desc.version);
    }
    cJSON *ota = cJSON_AddObjectToObject(res, "ota");
    cJSON_AddStringToObject(ota, "state", s_state);
    if (s_target[0]) {
        cJSON_AddStringToObject(ota, "version", s_target);
    }
    if (s_progress >= 0) {
        cJSON_AddNumberToObject(ota, "progress", s_progress);
    }
    if (s_error[0]) {
        cJSON_AddStringToObject(ota, "error", s_error);
    }
}

void ota_report_update(void)
{
    if (!s_report_pending || !mqtt_link_wait_ready(0)) {
        return;
    }
    const char *version = esp_app_get_description()->version;
    const char *partition = esp_ota_get_running_partition()->label;
    char previous[VERSION_LEN] = "unknown";
    esp_app_desc_t desc;
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (other && esp_ota_get_partition_description(other, &desc) == ESP_OK) {
        snprintf(previous, sizeof(previous), "%s", desc.version);
    }

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "version", version);
    cJSON_AddStringToObject(msg, "previous", previous);
    cJSON_AddStringToObject(msg, "partition", partition);
    if (clock_valid()) {
        cJSON_AddNumberToObject(msg, "time", (double)time(NULL));
    }
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!json || mqtt_link_publish_own("firmware", json, true) != ESP_OK) {
        free(json);
        return;
    }
    free(json);
    s_report_pending = false;
}
