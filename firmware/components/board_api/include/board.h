#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hal/gpio_types.h"

/*
 * Board support interface. Every board under boards/<name>/ provides a
 * component named `board` that implements this header, ui_status.h and
 * audio_input.h.
 */

typedef struct {
    gpio_num_t gpio;   /* active low */
    const char *name;  /* BLE button role, e.g. "primary" */
} board_button_t;

/* Called from ISR or task context when the charger or USB state may have changed. */
typedef void (*board_power_event_cb_t)(void);

/* Optional IMU stream (3-axis accelerometer or 6-axis accel + gyro).
 * read_samples writes little-endian int16 values:
 *   - 6 bytes (ax, ay, az) when include_gyro is false
 *   - 12 bytes (ax, ay, az, gx, gy, gz) when include_gyro is true */
typedef struct {
    bool has_gyro;
    uint16_t accel_lsb_per_g;
    float gyro_lsb_per_dps;
    uint16_t max_rate_hz;
    bool (*set_enabled)(bool enabled, bool include_gyro, uint16_t rate_hz);
    bool (*read_samples)(bool include_gyro, uint8_t *out_bytes);
} board_imu_t;
const board_imu_t *board_imu(void);

/* True if the board has a screen controlled by Protocol v2 display.set. */
bool board_has_display(void);

/* Hardware id sent in device.get_info; the desktop app only offers OTA images
 * whose manifest carries the same id. */
const char *board_hardware_name(void);

/* The first button is the talk button; it also wakes the board from deep sleep. */
size_t board_buttons(const board_button_t **buttons);

esp_err_t board_init(void);

esp_err_t board_power_events_init(board_power_event_cb_t callback);
/* Call from task context after the callback fired. */
void board_power_events_rearm(void);

esp_err_t board_battery_level(int *level_percent);
esp_err_t board_battery_charging(bool *charging);
esp_err_t board_usb_powered(bool *usb_powered);
/* True when the battery is too low to keep running without external power. */
bool board_battery_critical(void);

/*
 * Power down board peripherals and arm the deep-sleep wake sources: the talk
 * button and USB, or USB only. An error means sleep must be aborted, for
 * example because the talk button is still held.
 */
esp_err_t board_prepare_deep_sleep(bool usb_wake_only);
