idf_component_register(SRCS "wifi_estacion_main.c" "red_wifi.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash
                                lwip esp_timer)
