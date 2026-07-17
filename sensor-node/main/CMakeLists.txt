idf_component_register(
    SRCS "main.c" "sensors.c" "dht.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_adc esp_timer esp_driver_gpio esp_driver_i2c esp_driver_uart
)