#pragma once

#include <stdbool.h>

#include "board.h"
#include "driver/i2c_master.h"

bool magiclick_2p5_has_gc9107(void);
const board_confirmation_motion_t *magiclick_2p5_init_shake_sensor(i2c_master_bus_handle_t bus);
