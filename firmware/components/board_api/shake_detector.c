#include "shake_detector.h"

enum {
    MIN_TURN_US = 55000,
    MAX_SHAKE_US = 600000,
    MIN_DELTA_MG = 640,
};

static int32_t dot(acceleration_mg_t a, acceleration_mg_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/* Three alternating impulses distinguish a shake from a tilt, tap, or pickup. */
bool shake_detector_sample(shake_detector_t *detector, acceleration_mg_t sample, int64_t now_us)
{
    const acceleration_mg_t previous = detector->previous;
    detector->previous = sample;
    if (!detector->has_previous) {
        detector->has_previous = true;
        return false;
    }
    if (now_us - detector->started_us > MAX_SHAKE_US) {
        detector->peaks = 0;
    }

    const acceleration_mg_t delta = {sample.x - previous.x, sample.y - previous.y, sample.z - previous.z};
    const int32_t threshold = MIN_DELTA_MG * MIN_DELTA_MG;
    if (dot(delta, delta) < threshold) {
        return false;
    }
    if (detector->peaks == 0) {
        detector->started_us = now_us;
    } else if (now_us - detector->last_peak_us < MIN_TURN_US || dot(delta, detector->peak) > -threshold / 2) {
        return false;
    }
    detector->last_peak_us = now_us;
    detector->peak = delta;
    if (++detector->peaks < 3) {
        return false;
    }
    detector->peaks = 0;
    return true;
}
