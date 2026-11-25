idf_component_register(
    SRCS "board.c"
    REQUIRES board_api ui_status es8311_input
    PRIV_REQUIRES bmi270 stick_s3_board esp_driver_gpio esp_hw_support esp_rom
)
