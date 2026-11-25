#include "board_lcd.h"

#include "driver/gpio.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_st7789.h"

#include "magiclick_2p5.h"

/* The 2.5A's GC9107 uses the GC9A01 transport with this initialization table
 * from the upstream xiaozhi-esp32 Magiclick 2P5 board. */
static const gc9a01_lcd_init_cmd_t s_gc9107_init[] = {
    {0xfe, NULL, 0, 0},
    {0xef, NULL, 0, 0},
    {0xb0, (const uint8_t[]){0xc0}, 1, 0},
    {0xb1, (const uint8_t[]){0x80}, 1, 0},
    {0xb2, (const uint8_t[]){0x27}, 1, 0},
    {0xb3, (const uint8_t[]){0x13}, 1, 0},
    {0xb6, (const uint8_t[]){0x19}, 1, 0},
    {0xb7, (const uint8_t[]){0x05}, 1, 0},
    {0xac, (const uint8_t[]){0xc8}, 1, 0},
    {0xab, (const uint8_t[]){0x0f}, 1, 0},
    {0x3a, (const uint8_t[]){0x05}, 1, 0},
    {0xb4, (const uint8_t[]){0x04}, 1, 0},
    {0xa8, (const uint8_t[]){0x08}, 1, 0},
    {0xb8, (const uint8_t[]){0x08}, 1, 0},
    {0xea, (const uint8_t[]){0x02}, 1, 0},
    {0xe8, (const uint8_t[]){0x2a}, 1, 0},
    {0xe9, (const uint8_t[]){0x47}, 1, 0},
    {0xe7, (const uint8_t[]){0x5f}, 1, 0},
    {0xc6, (const uint8_t[]){0x21}, 1, 0},
    {0xc7, (const uint8_t[]){0x15}, 1, 0},
    {0xf0, (const uint8_t[]){0x1d, 0x38, 0x09, 0x4d, 0x92, 0x2f, 0x35,
                             0x52, 0x1e, 0x0c, 0x04, 0x12, 0x14, 0x1f}, 14, 0},
    {0xf1, (const uint8_t[]){0x16, 0x40, 0x1c, 0x54, 0xa9, 0x2d, 0x2e,
                             0x56, 0x10, 0x0d, 0x0c, 0x1a, 0x14, 0x1e}, 14, 0},
    {0xf4, (const uint8_t[]){0x00, 0x00, 0xff}, 3, 0},
    {0xba, (const uint8_t[]){0xff, 0xff}, 2, 0},
};

static esp_err_t new_panel(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t *panel)
{
    const bool gc9107 = magiclick_2p5_has_gc9107();
    gc9a01_vendor_config_t gc9107_config = {
        .init_cmds = s_gc9107_init,
        .init_cmds_size = sizeof(s_gc9107_init) / sizeof(s_gc9107_init[0]),
    };
    const esp_lcd_panel_dev_config_t config = {
        .reset_gpio_num = GPIO_NUM_17,
        .rgb_ele_order = gc9107 ? LCD_RGB_ELEMENT_ORDER_RGB : LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .vendor_config = gc9107 ? &gc9107_config : NULL,
    };
    if (gc9107) {
        return esp_lcd_new_panel_gc9a01(io, &config, panel);
    }
    /* ST7735 uses the same SPI commands as the upstream ST7789 driver path. */
    return esp_lcd_new_panel_st7789(io, &config, panel);
}

board_lcd_t board_lcd_config(void)
{
    const bool gc9107 = magiclick_2p5_has_gc9107();
    return (board_lcd_t){
        .host = SPI3_HOST,
        .sck = GPIO_NUM_15,
        .mosi = GPIO_NUM_16,
        .cs = GPIO_NUM_14,
        .dc = GPIO_NUM_18,
        .backlight = GPIO_NUM_13,
        .backlight_inverted = true,
        .width = 128,
        .height = 128,
        .gap_x = gc9107 ? 0 : 2,
        .gap_y = gc9107 ? 0 : 3,
        .mirror = !gc9107,
        .invert_colors = !gc9107,
        .new_panel = new_panel,
    };
}
