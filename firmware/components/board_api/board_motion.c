#include "board.h"

__attribute__((weak)) const board_imu_t *board_imu(void)
{
    return NULL;
}

__attribute__((weak)) bool board_has_display(void)
{
    return false;
}
