/*
 * rmx-mower-gateway: LilyGo T-SIM7600 (ESP32 + SIM7600G-H) talking to
 * rmx-server-app over LTE, on the topics of wiki Mower-Protocol (mower/<name>/gateway/...).
 *
 * board -> cell (PPP) -> MQTT (TLS, client certificate; retained status, last will)
 * -> stay online: state, metric catalogue and telemetry (main/metrics.c), remote
 * commands from gateway/cmd (main/remote_cmd.c). Firmware updates arrive as
 * MQTT chunks (main/ota.c).
 */
#include <stdio.h>
#include <sys/time.h>
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
#include "settings.h"
#include "board.h"
#include "cell.h"
#include "creds.h"
#include "devcert.h"
#include "gw_link.h"
#include "gwstate.h"
#include "metrics.h"
#include "mqtt_link.h"
#include "ota.h"
#include "remote_cmd.h"
#include "supervisor.h"
#include "sdkconfig.h"

#if !CONFIG_MBEDTLS_HAVE_TIME_DATE
/* An older sdkconfig.gateway keeps it off over sdkconfig.defaults: delete that line. */
#error "CONFIG_MBEDTLS_HAVE_TIME_DATE must be y: TLS would ignore certificate dates"
#endif

static const char *TAG = "gateway";

static void restart_in_30s(const char *why)
{
    ESP_LOGE(TAG, "%s", why);
    gw_restart(why, 30000);
}

static bool connect_mqtt(void)
{
    if (mqtt_link_start() != ESP_OK || !mqtt_link_wait_ready(90000)) {
        ESP_LOGE(TAG, "mqtt connect: FAIL");
        return false;
    }
    ESP_LOGI(TAG, "mqtt connect: PASS");   /* the supervisor confirms a new image */
    return true;
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s %s (IDF %s)", app->project_name, app->version, app->idf_ver);

    ESP_ERROR_CHECK(settings_init());
    gw_boot_init();
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
    ESP_ERROR_CHECK(gw_link_init());

    board_init();
    if (cell_start(60000) != ESP_OK && !cell_wait_ip(180000)) {
        restart_in_30s("no data connection");
    }

    /* TLS checks certificate dates (CONFIG_MBEDTLS_HAVE_TIME_DATE), so no MQTT before
     * SNTP has set the clock; it also drives command expiry (expires_at). */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    for (int tries = 1; esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK; tries++) {
        if (tries == 8) {
            restart_in_30s("SNTP: no time after 2 min");
        }
        ESP_LOGW(TAG, "SNTP: no time yet, not starting TLS");
    }
    ESP_LOGI(TAG, "clock set by SNTP: %lld", (long long)time(NULL));
    gw_link_set(GW_CLOCK_VALID, true);
#if CONFIG_GATEWAY_TEST_CLOCK_SHIFT_DAYS != 0
    /* Test only (wiki Mower-Gateway-Plan, certificate dates): a wrong clock must make TLS refuse the broker. */
    esp_netif_sntp_deinit();
    struct timeval tv = { .tv_sec = time(NULL) + CONFIG_GATEWAY_TEST_CLOCK_SHIFT_DAYS * 86400LL };
    settimeofday(&tv, NULL);
    ESP_LOGW(TAG, "TEST: clock shifted by %d days to %lld", CONFIG_GATEWAY_TEST_CLOCK_SHIFT_DAYS,
             (long long)tv.tv_sec);
#endif

    ESP_ERROR_CHECK(remote_cmd_init());
    ota_resume_pending();
    metrics_start();   /* catalogue, telemetry and monitor streams once the server link is up */
    ESP_ERROR_CHECK(supervisor_start());   /* state, image confirm, OTA result, alive log */
    bool online = connect_mqtt();
    ESP_LOGI(TAG, "STARTUP %s, free heap %u, min %u, internal %u", online ? "OK" : "FAILED",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    /* app_main returns: esp-mqtt reconnects by itself, cell redials on PPP loss,
     * the supervisor acts on link changes. */
}
