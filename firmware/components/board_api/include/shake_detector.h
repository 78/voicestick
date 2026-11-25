#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int32_t x, y, z;
} acceleration_mg_t;

typedef struct {
    bool has_previous;
    acceleration_mg_t previous;
    acceleration_mg_t peak;
    unsigned peaks;
    int64_t started_us;
    int64_t last_peak_us;
} shake_detector_t;

bool shake_detector_sample(shake_detector_t *detector, acceleration_mg_t sample, int64_t now_us);
