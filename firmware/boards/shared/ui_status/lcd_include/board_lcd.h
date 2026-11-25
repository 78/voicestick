#pragma once

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

typedef struct {
    spi_host_device_t host;
    int sck;
    int mosi;
    int cs;
    int dc;
    int backlight;
    bool backlight_inverted;
    int width;
    int height;
    int gap_x;
    int gap_y;
    bool mirror;
    bool invert_colors;
    esp_err_t (*new_panel)(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t *panel);
} board_lcd_t;

board_lcd_t board_lcd_config(void);
