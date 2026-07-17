idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_uart nvs_flash esp_wifi esp_event esp_netif mqtt esp_driver_gpio
    EMBED_TXTFILES "ca.crt"
)

