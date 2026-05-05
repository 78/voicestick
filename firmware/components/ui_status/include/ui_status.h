#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t ui_status_init(void);
esp_err_t ui_status_set_brightness(uint8_t brightness);
void ui_status_prepare_deep_sleep(void);
void ui_status_set_advertising(void);
void ui_status_set_pairing(const char *device_name);
void ui_status_set_idle(void);
void ui_status_set_idle_dimmed(bool dimmed);
void ui_status_set_recording(uint32_t session_id);
void ui_status_set_battery(int level_percent, bool charging, bool usb_powered);
void ui_status_set_partial_text(const char *text);
void ui_status_set_ota_progress(uint32_t written, uint32_t size);
void ui_status_set_ota_rebooting(void);
void ui_status_set_error(const char *message);
