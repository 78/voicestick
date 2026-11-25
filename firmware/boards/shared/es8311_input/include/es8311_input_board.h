#pragma once

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

typedef struct {
    i2c_port_num_t i2c_port;
    i2c_master_bus_handle_t bus;
    int i2s_port;
    i2s_std_gpio_config_t pins;
    float gain_db;
} es8311_input_board_t;

es8311_input_board_t board_es8311_input_config(void);
