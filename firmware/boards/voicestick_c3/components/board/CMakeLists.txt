idf_component_register(
    SRCS "board.c" "audio_input.c" "status_led.c"
    PRIV_INCLUDE_DIRS "."
    REQUIRES board_api
    PRIV_REQUIRES esp_adc esp_driver_gpio esp_driver_i2s esp_hw_support esp_pm esp_rom esp_timer led_strip
)
