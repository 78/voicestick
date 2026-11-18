idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board voice_ble audio_pipeline button esp_driver_gpio esp_pm esp_timer cjson
)
