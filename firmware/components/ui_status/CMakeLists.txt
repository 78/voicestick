set(srcs "ui_status.c" "ui_status_icons.c" "ui_cards.c" "ui_control.c" "ui_history_text.c" "ui_weather.c"
         "ui_face_picker.c" "ui_options.c"
         "fonts/todoteck_font_16.c" "fonts/todoteck_font_14.c"
         "fonts/todoteck_font_13.c" "fonts/todoteck_font_12.c"
         "fonts/todoteck_font_11.c" "fonts/todoteck_font_10.c"
         "fonts/todoteck_clock_64.c")

# Die Uhr bringt ihren eigenen Displaytreiber und groessere Schriften mit;
# im StickS3-Abbild haetten sie nichts zu suchen.
if(CONFIG_VOICE_BOARD_WATCH206)
    list(APPEND srcs "ui_display_watch206.c"
         "fonts/todoteck_watch_font_30.c" "fonts/todoteck_watch_font_26.c"
         "fonts/todoteck_watch_font_23.c" "fonts/todoteck_watch_font_20.c"
         "fonts/todoteck_watch_font_18.c" "fonts/todoteck_watch_clock_120.c"
         "fonts/todoteck_watch_clock_180.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" "."
    REQUIRES driver esp_driver_ledc esp_lcd esp_timer nvs_flash stick_s3_board voice_board lvgl
)
