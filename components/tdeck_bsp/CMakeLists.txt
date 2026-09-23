idf_component_register(
    SRCS
        "src/tdeck_bsp.c"
        "src/tdeck_power.c"
        "src/tdeck_display.c"
        "src/tdeck_input.c"
        "src/tdeck_touch.c"
        "src/tdeck_audio.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_ledc
        esp_lcd esp_adc esp_timer esp_driver_i2s
        espressif__esp_lcd_touch espressif__esp_lcd_touch_gt911
)
