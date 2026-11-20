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
#include "fw_chunk.h"
#include "cell.h"
#include "gwstate.h"
#include "mqtt_link.h"
#include "remote_cmd.h"
#include "supervisor.h"
#include "esp_random.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

#define REQUEST_ID_LEN 32
/* Project name of images built before the rename; one of them may update to this project. */
#define LEGACY_PROJECT_NAME "rmx_mower_modem_probe"

/* A new request id for every firmware request ("dev-<random>-<n>", wiki Mower-Protocol §12.1). */
static void new_request_id(char buf[REQUEST_ID_LEN + 1])
{
    static uint32_t boot_tag;
    static uint32_t counter;
    if (!boot_tag) {
        boot_tag = esp_random() | 1;
    }
    snprintf(buf, REQUEST_ID_LEN + 1, "dev-%08lx-%lu", (unsigned long)boot_tag, (unsigned long)++counter);
}

/* Chunk transfer, wiki Mower-Protocol §12: one request open at a time; the
 * board waits 10 s for the answer, then asks for the same chunk again with a
 * new request id, up to 5 times in a row, then the download has failed. Bytes
 * already written with esp_ota_write stay, so a retry resumes at that chunk. */
#define CHUNK_TRIES     6   /* the request and 5 repeats */
#define CHUNK_WAIT_MS   10000
#define ID_LEN          64
#define FILE_LEN        128
#define VERSION_LEN     32
#define FW_MAX_FRAME    (FW_CHUNK_HDR_LEN + 16384)

#define APP_DESC_BYTES  (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

/* The 64-byte chunk header (§12.2) is parsed by components/rmx_codec (fw_chunk_parse, host-tested). */

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
static volatile int s_progress = -1;

/* NVS "gw": the open ota command across the restart (§6.5, steps 2 to 4). */
#define NVS_OTA_ID   "ota_id"
#define NVS_OTA_VER  "ota_ver"    /* the version the command installs */
#define NVS_OTA_PREV "ota_prev"   /* the version that ran before */

/* Final ack "failed": the update ends here, the old image keeps running. */
static void fail(const char *error, const char *message)
{
    ESP_LOGE(TAG, "update to %s failed: %s (%s)", s_req.version, error, message);
    cmd_ack(s_req.id, "ota", "failed", error, message, NULL);
}

/* Both firmware routes; ctx is non-NULL for fw/chunk (binary), NULL for fw/res (JSON). */
static void on_fw(const char *subtopic, const uint8_t *data, size_t len, void *ctx)
{
    bool chunk = ctx != NULL;
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
    drain_rx();
    s_busy = false;
    vTaskDelete(NULL);
    abort();
}

static void restart_into_new_image(void)
{
    char why[48];
    snprintf(why, sizeof(why), "into %s", s_req.version);
    gw_restart(why, 3000);
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
                            uint8_t **frame, fw_chunk_hdr_t *hdr, const uint8_t **payload,
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
        if (!fw_chunk_parse(m.data, m.len, expect_hash, hdr, payload)) {
            free(m.data);   /* not ours, or not a version-1 header */
            continue;
        }
        *frame = m.data;
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

static bool fetch_chunk(uint32_t index, uint8_t **frame, fw_chunk_hdr_t *hdr, const uint8_t **payload)
{
    for (int attempt = 0; attempt < CHUNK_TRIES; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "chunk %lu: no answer, asking again (%d/%d)", (unsigned long)index,
                     attempt, CHUNK_TRIES - 1);
        }
        if (!link_up(attempt)) {
            continue;
        }
        char rid[REQUEST_ID_LEN + 1];
        new_request_id(rid);   /* a new id for every request */
        uint8_t expect[8];
        if (!sha256_id(rid, expect)) {
            fail("failed", "could not hash the firmware request id");
            return false;
        }
        char body[FILE_LEN + 80];
        snprintf(body, sizeof(body), "{\"request_id\":\"%s\",\"file\":\"%s\",\"chunk\":%lu}",
                 rid, s_req.file, (unsigned long)index);
        if (mqtt_link_publish_fw_req(body) != ESP_OK) {
            continue;
        }
        char code[24];
        char message[96];
        rx_kind_t kind = wait_chunk(rid, expect, CHUNK_WAIT_MS, frame, hdr, payload,
                                    code, sizeof(code), message, sizeof(message));
        if (kind == RX_CHUNK) {
            return true;
        }
        if (kind == RX_SERVER) {
            /* An error answer ends the download (§12.4). */
            char text[160];
            snprintf(text, sizeof(text), "the server answered %s: %s", code, message);
            fail("download_failed", text);
            return false;
        }
    }
    fail("download_failed", "no firmware chunk (the link dropped or firmware is not served)");
    return false;
}

static bool check_desc(const esp_partition_t *part)
{
    esp_app_desc_t desc;
    const esp_app_desc_t *running = esp_app_get_description();
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) {
        fail("bad_image", "could not read the image header");
        return false;
    }
    ESP_LOGI(TAG, "image: %s %s (IDF %s)", desc.project_name, desc.version, desc.idf_ver);
    bool renamed = strncmp(running->project_name, LEGACY_PROJECT_NAME, sizeof(desc.project_name)) == 0 &&
                   strncmp(desc.project_name, "rmx_mower_gateway", sizeof(desc.project_name)) == 0;
    if (!renamed && strncmp(desc.project_name, running->project_name, sizeof(desc.project_name)) != 0) {
        fail("bad_image", "the image is for another project");
        return false;
    }
    if (strncmp(desc.version, s_req.version, sizeof(desc.version)) != 0) {
        fail("bad_image", "the image has another version than the command");
        return false;
    }
    if (strncmp(desc.version, running->version, sizeof(desc.version)) == 0) {
        fail("same_version", "this version is already running");
        return false;
    }
    return true;
}

static void ota_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    s_listen = true;
    drain_rx();
    if (psa_crypto_init() != PSA_SUCCESS) {
        fail("failed", "crypto init failed");
        finish_task();
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        fail("failed", "no OTA partition");
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
        fw_chunk_hdr_t hdr;
        const uint8_t *payload = NULL;
        if (!fetch_chunk(index, &frame, &hdr, &payload)) {
            if (handle) {
                esp_ota_abort(handle);
            }
            if (sha_open) {
                psa_hash_abort(&sha);
            }
            finish_task();
        }

        if (hdr.chunk_index != index) {
            free(frame);
            fail("bad_image", "firmware chunk index does not match the request");
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
                fail("bad_size", "firmware image size is not usable");
                finish_task();
            }
            if (s_req.size > 0 && (uint32_t)s_req.size != image_size) {
                free(frame);
                ESP_LOGE(TAG, "image size %lu, expected %d", (unsigned long)image_size, s_req.size);
                fail("bad_size", "the image size differs from the command");
                finish_task();
            }
            if (psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
                free(frame);
                fail("failed", "could not start the image hash");
                finish_task();
            }
            sha_open = true;
            if (esp_ota_begin(part, image_size, &handle) != ESP_OK) {
                free(frame);
                psa_hash_abort(&sha);
                fail("failed", "could not start writing the update slot");
                finish_task();
            }
            have_meta = true;
        } else if (hdr.image_size != image_size || hdr.chunk_count != chunk_count ||
                   memcmp(hdr.sha256, expect_sha, sizeof(expect_sha)) != 0) {
            free(frame);
            esp_ota_abort(handle);
            psa_hash_abort(&sha);
            fail("bad_image", "firmware chunk header changed mid-download");
            finish_task();
        }

        if (hdr.chunk_len > 0) {
            if (esp_ota_write(handle, payload, hdr.chunk_len) != ESP_OK ||
                    psa_hash_update(&sha, payload, hdr.chunk_len) != PSA_SUCCESS) {
                free(frame);
                esp_ota_abort(handle);
                psa_hash_abort(&sha);
                fail("failed", "writing the update slot failed");
                finish_task();
            }
        }
        written += hdr.chunk_len;
        free(frame);

        if (!desc_checked && written >= APP_DESC_BYTES) {
            if (!check_desc(part)) {
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
        fail("bad_image", "image is too small to hold an app header");
        finish_task();
    }
    if (written != image_size) {
        esp_ota_abort(handle);
        psa_hash_abort(&sha);
        fail("bad_size", "the chunks do not add up to the image size");
        finish_task();
    }

    uint8_t got[32];
    size_t got_len = 0;
    psa_status_t st = psa_hash_finish(&sha, got, sizeof(got), &got_len);
    sha_open = false;
    if (st != PSA_SUCCESS || got_len != sizeof(expect_sha) || memcmp(got, expect_sha, sizeof(got)) != 0) {
        esp_ota_abort(handle);
        fail("bad_image", "image SHA-256 does not match the firmware header");
        finish_task();
    }
    ESP_LOGI(TAG, "image SHA-256 matches the chunk header");

    /* esp_ota_end checks the image, including the RSA-3072 signature when
     * CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT is set. The handle is
     * released here, success or not. Boot slot changes only after that. */
    esp_err_t err = esp_ota_end(handle);
    handle = 0;
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        fail("bad_image", "image check failed (signature or hash, see the device log)");
        finish_task();
    }
    if (err != ESP_OK) {
        fail("failed", esp_err_to_name(err));
        finish_task();
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        fail("failed", esp_err_to_name(err));
        finish_task();
    }

    ESP_LOGI(TAG, "update to %s written in %lld s", s_req.version, (esp_timer_get_time() - t0) / 1000000);
    s_progress = 100;
    /* §6.5 step 2: the command id is kept across the restart. The new image
     * sends the final ack (done, or rolled_back from the old image). */
    gw_nvs_set(NVS_OTA_ID, s_req.id);
    gw_nvs_set(NVS_OTA_VER, s_req.version);
    gw_nvs_set(NVS_OTA_PREV, esp_app_get_description()->version);
    restart_into_new_image();
    finish_task();
}

/* A file name is 1-128 characters of [A-Za-z0-9._-], never a path (§12). */
static bool valid_fw_file(const char *name)
{
    size_t n = strlen(name);
    if (n < 1 || n > FILE_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
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
        *message = "file must be 1-128 characters of A-Za-z0-9._-";
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
    s_progress = 0;
    /* The "accepted" ack goes out before the task starts, so a failure of the
     * task cannot overtake it. */
    cmd_ack(cmd_id, "ota", "accepted", NULL, NULL, NULL);
    if (xTaskCreate(ota_task, "ota", 12288, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
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

/* The ota command (§6.4): arguments checked here, the update runs on its own task. */
static void cmd_ota(const char *id, const cJSON *cmd, cmd_reply_t *r)
{
    const cJSON *size_item = cJSON_GetObjectItem(cmd, "size");
    const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "version"));
    const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "file"));
    char file_buf[129];
    r->error = CMD_E_BAD_REQUEST;
    if (!file && cJSON_GetObjectItem(cmd, "file")) {
        r->message = "file must be a string";
    } else if (size_item && (!cJSON_IsNumber(size_item) || cJSON_GetNumberValue(size_item) < 0)) {
        r->message = "size must be the image size in bytes";
    } else if (!version || !version[0]) {
        r->message = "ota needs a string version";
    } else {
        if (!file || !file[0]) {
            snprintf(file_buf, sizeof(file_buf), "%s.bin", version);
            file = file_buf;
        }
        /* ota_start sends the "accepted" ack itself, before the task can fail */
        if (ota_start(id, file, version, size_item ? (int)cJSON_GetNumberValue(size_item) : 0, &r->error,
                      &r->message)) {
            r->error = NULL;
            r->accepted = true;
        }
    }
}

void ota_boot_init(void)
{
    cmd_register("ota", cmd_ota, 0);
    /* Subscribed before the first fw/req: mqtt_link_wait_ready() waits for both. */
    mqtt_link_route(MQTT_SCOPE_GATEWAY, "fw/chunk", 1, MQTT_ROUTE_READY, on_fw, (void *)1);
    mqtt_link_route(MQTT_SCOPE_GATEWAY, "fw/res", 1, MQTT_ROUTE_READY, on_fw, NULL);
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

void ota_resume_pending(void)
{
    char id[ID_LEN + 1];
    if (gw_nvs_get(NVS_OTA_ID, id, sizeof(id))) {
        /* a resend of the command's id gets "accepted" until the final ack is out */
        cmd_remember_accepted(id, "ota");
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
        ESP_LOGI(TAG, "image %s confirmed", esp_app_get_description()->version);
    }
}

/* §6.5 steps 3 and 4: the final ack of an update that restarted the board. */
bool ota_report_result(void)
{
    char id[ID_LEN + 1], target[VERSION_LEN], previous[VERSION_LEN];
    if (!gw_nvs_get(NVS_OTA_ID, id, sizeof(id)) || !gw_nvs_get(NVS_OTA_VER, target, sizeof(target))) {
        return true;   /* no update was open */
    }
    if (s_pending_verify || !mqtt_link_wait_ready(0)) {
        return false;
    }
    if (!gw_nvs_get(NVS_OTA_PREV, previous, sizeof(previous))) {
        snprintf(previous, sizeof(previous), "unknown");
    }
    const char *version = esp_app_get_description()->version;
    if (strcmp(version, target) == 0) {
        /* the new image runs, confirmed and connected */
        gw_nvs_set("prev", previous);
        gw_nvs_set("prev_of", version);
        cJSON *res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "version", version);
        cJSON_AddStringToObject(res, "previous", previous);
        cJSON_AddStringToObject(res, "partition", esp_ota_get_running_partition()->label);
        gw_state_publish(true);   /* before the ack: the state already names the new version */
        cmd_ack(id, "ota", "done", NULL, NULL, res);
    } else {
        char text[96];
        snprintf(text, sizeof(text), "%s did not confirm; %s runs again", target, version);
        cmd_ack(id, "ota", "failed", "rolled_back", text, NULL);
    }
    gw_nvs_erase(NVS_OTA_ID);
    gw_nvs_erase(NVS_OTA_VER);
    gw_nvs_erase(NVS_OTA_PREV);
    return true;
}
