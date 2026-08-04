idf_component_register(
    SRCS "app_main.c" "app_ir.c" "app_oled.c" "app_ui.c" "font5x7.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_driver_i2c esp_driver_rmt esp_driver_gpio freertos spiffs esp_timer
)
