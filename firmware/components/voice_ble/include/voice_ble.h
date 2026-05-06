#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define VOICE_BLE_DEVICE_NAME_PREFIX "VS"

#define VOICE_BLE_FLAG_START 0x01
#define VOICE_BLE_FLAG_END   0x02

#define VOICE_BLE_OTA_TYPE_BEGIN 0x20
#define VOICE_BLE_OTA_TYPE_DATA  0x21
#define VOICE_BLE_OTA_TYPE_END   0x22
#define VOICE_BLE_OTA_TYPE_ABORT 0x23
#define VOICE_BLE_OTA_TYPE_STATE 0x30

typedef void (*voice_ble_connection_cb_t)(bool connected);
typedef void (*voice_ble_control_cb_t)(const char *json);

typedef enum {
    VOICE_BLE_OTA_EVENT_BEGIN,
    VOICE_BLE_OTA_EVENT_PROGRESS,
    VOICE_BLE_OTA_EVENT_DONE,
    VOICE_BLE_OTA_EVENT_ERROR,
    VOICE_BLE_OTA_EVENT_ABORT,
} voice_ble_ota_event_t;

typedef void (*voice_ble_ota_cb_t)(voice_ble_ota_event_t event,
                                   uint32_t written,
                                   uint32_t size);

esp_err_t voice_ble_init(void);
void voice_ble_set_connection_callback(voice_ble_connection_cb_t callback);
void voice_ble_set_control_callback(voice_ble_control_cb_t callback);
void voice_ble_set_ota_callback(voice_ble_ota_cb_t callback);
const char *voice_ble_device_id(void);
const char *voice_ble_device_name(void);
bool voice_ble_is_connected(void);
bool voice_ble_is_ready(void);
bool voice_ble_ota_is_active(void);
esp_err_t voice_ble_send_audio(uint32_t session_id, uint32_t seq, uint8_t flags,
                               const uint8_t *opus_payload, size_t len);
esp_err_t voice_ble_send_device_info(void);
esp_err_t voice_ble_send_button_down(const char *button, uint32_t session_id);
esp_err_t voice_ble_send_button_up(const char *button, uint32_t duration_ms,
                                   uint32_t session_id);
