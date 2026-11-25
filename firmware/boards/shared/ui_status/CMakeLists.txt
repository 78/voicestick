idf_component_register(
    SRCS "ui_status.c" "ui_status_icons.c"
    INCLUDE_DIRS "lcd_include"
    EMBED_FILES
        "assets/cat_pairing_argb8888.bin"
        "assets/cat_ready_argb8888.bin"
        "assets/cat_listening_argb8888.bin"
        "assets/cat_thinking_argb8888.bin"
        "assets/cat_resting_argb8888.bin"
        "assets/cat_error_argb8888.bin"
    REQUIRES board_api esp_driver_spi esp_lcd
    PRIV_REQUIRES esp_driver_ledc esp_timer lvgl
)

# The selected board supplies its panel constructor and wiring.
idf_component_add_link_dependency(FROM ui_status TO board)
