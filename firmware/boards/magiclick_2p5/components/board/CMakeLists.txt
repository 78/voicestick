idf_component_register(
    SRCS "board.c" "lcd.c" "shake.c"
    REQUIRES board_api es8311_input ui_status
    PRIV_REQUIRES adc_battery_estimation esp_adc esp_driver_gpio esp_driver_usb_serial_jtag esp_hw_support esp_lcd_gc9a01
)
