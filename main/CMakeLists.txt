idf_component_register(SRCS "app_main.c" "board.c" "cell.c" "mqtt_link.c" "remote_cmd.c" "ota.c" "creds.c" "devcert.c"
                            "usage.c" "gwstate.c" "metrics.c" "monitor.c" "gw_link.c" "supervisor.c" "gw_tasks.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_netif esp_event esp_timer
                                     esp_hw_support mbedtls nvs_flash app_update
                                     bootloader_support esp_driver_uart lwip rmx_codec)

# usage.c counts the IP packets lwIP's PPP code receives (it calls ip4_input directly).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ip4_input")
