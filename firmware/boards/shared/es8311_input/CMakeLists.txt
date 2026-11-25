idf_component_register(
    SRCS "audio_input.c"
    INCLUDE_DIRS "include"
    REQUIRES board_api esp_driver_i2s esp_driver_i2c
    PRIV_REQUIRES esp_codec_dev
)

# The selected board supplies the microphone pin and bus configuration.
idf_component_add_link_dependency(FROM es8311_input TO board)
