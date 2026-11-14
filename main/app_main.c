/*
 * rmx-mower-gateway: LilyGo T-SIM7600 (ESP32 + SIM7600G-H) talking to
 * rmx-server-app over LTE.
 *
 * board -> cell (PPP) -> MQTT (TLS, client certificate; retained status, last will)
 * -> stay online and run remote commands from mower/<name>/cmd (see remote_cmd.h).
 * Firmware updates arrive as MQTT chunks (main/ota.c).
 */
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "board.h"
#include "cell.h"
#include "creds.h"
#include "devcert.h"
#include "gnss.h"
#include "mqtt_link.h"
#include "ota.h"
#include "remote_cmd.h"
#include "sdkconfig.h"

static const char *TAG = "gateway";

static bool connect_mqtt(void)
{
    if (mqtt_link_start() != ESP_OK || !mqtt_link_wait_ready(90000)) {
        ESP_LOGE(TAG, "mqtt connect: FAIL");
        return false;
    }
    ESP_LOGI(TAG, "mqtt connect: PASS");
    ota_confirm();   /* a new image that reaches the broker is good */
    ota_report_update();
    return true;
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s %s (IDF %s)", app->project_name, app->version, app->idf_ver);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    devcert_init();
    devcert_console_start();
    if (!devcert_cn()) {
        /* No identity, no connection: wait for tools/provision_cert.ps1 (RMX-CSR <name>, then the chain). */
        for (;;) {
            ESP_LOGE(TAG, "no client certificate: not connecting. Run tools/provision_cert.ps1 -Name <name>");
            vTaskDelay(pdMS_TO_TICKS(30000));
        }
    }
    ESP_LOGI(TAG, "booting as %s", creds_device_name());
    ota_boot_init();   /* a new image rolls back unless MQTT connects in time */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    board_init();
    if (cell_start(60000) != ESP_OK && !cell_wait_ip(180000)) {
        ESP_LOGE(TAG, "no data connection, restarting in 30 s");
        vTaskDelay(pdMS_TO_TICKS(30000));
        cell_stop();
        esp_restart();
    }

    /* Clock for command expiry (expires_at) and TLS logs; not fatal if it fails. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK) {
        ESP_LOGI(TAG, "clock set by SNTP: %lld", (long long)time(NULL));
    } else {
        ESP_LOGW(TAG, "SNTP: no time yet (commands with expires_at are refused until it syncs)");
    }

    gnss_start();   /* the first fix takes a while: start searching early */

    ESP_ERROR_CHECK(remote_cmd_init());
    bool online = connect_mqtt();
    ESP_LOGI(TAG, "STARTUP %s, free heap %u, min %u, internal %u", online ? "OK" : "FAILED",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Stay online: esp-mqtt reconnects by itself, cell redials on PPP loss. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        if (mqtt_link_wait_ready(0)) {
            ota_confirm();   /* MQTT came up after the first try */
            ota_report_update();
        }
        ESP_LOGI(TAG, "alive: ppp %s, rssi %d, free heap %u", cell_is_up() ? "up" : "down",
                 cell_last_rssi(), (unsigned)esp_get_free_heap_size());
    }
}
