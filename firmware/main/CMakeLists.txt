idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES voice_board stick_s3_board voice_ble app_update audio_pipeline audio_tone ui_status wrist_wake button esp_pm esp_timer json mbedtls nvs_flash
)
