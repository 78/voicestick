#pragma once

#include <stdbool.h>

#include "board.h"
#include "driver/i2c_master.h"

bool magiclick_2p5_has_gc9107(void);
const board_imu_t *magiclick_2p5_init_imu_sensor(i2c_master_bus_handle_t bus);
