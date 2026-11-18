idf_component_register(
    SRCS "board.c" "audio_input.c"
    REQUIRES board_api ui_status
    PRIV_REQUIRES stick_s3_board esp_driver_gpio esp_driver_i2s esp_hw_support esp_codec_dev
)
