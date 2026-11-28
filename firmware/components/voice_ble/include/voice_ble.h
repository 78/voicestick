#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#define VOICE_BLE_DEVICE_NAME_PREFIX "VS"
#define VOICE_BLE_PROTOCOL_VERSION 2
#define VOICE_BLE_MAX_CONTROL_MESSAGE_BYTES 1024

/* Protocol v2 frame types */
#define VOICE_BLE_TYPE_NOP           0x00
#define VOICE_BLE_TYPE_AUDIO_IN      0x01
#define VOICE_BLE_TYPE_AUDIO_OUT     0x02
#define VOICE_BLE_TYPE_IMU           0x03
#define VOICE_BLE_TYPE_INPUT         0x04
#define VOICE_BLE_TYPE_CONTROL_JSON  0x10
#define VOICE_BLE_TYPE_CONTROL_CHUNK 0x1F

/* Audio stream flags */
#define VOICE_BLE_FLAG_START 0x01
#define VOICE_BLE_FLAG_END   0x02
#define VOICE_BLE_FLAG_FLUSH 0x04

/* OTA v1 frame types (unchanged for upgrade compatibility) */
#define VOICE_BLE_OTA_TYPE_BEGIN 0x20
#define VOICE_BLE_OTA_TYPE_DATA  0x21
#define VOICE_BLE_OTA_TYPE_END   0x22
#define VOICE_BLE_OTA_TYPE_ABORT 0x23
#define VOICE_BLE_OTA_TYPE_STATE 0x30

typedef void (*voice_ble_connection_cb_t)(bool connected);
typedef void (*voice_ble_pairing_cb_t)(bool open);
typedef void (*voice_ble_control_cb_t)(const char *json);
typedef void (*voice_ble_stream_rx_cb_t)(const uint8_t *data, size_t len);

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
void voice_ble_set_pairing_callback(voice_ble_pairing_cb_t callback);
void voice_ble_open_pairing(void);
void voice_ble_set_control_callback(voice_ble_control_cb_t callback);
void voice_ble_set_stream_rx_callback(voice_ble_stream_rx_cb_t callback);
void voice_ble_set_ota_callback(voice_ble_ota_cb_t callback);

const char *voice_ble_device_id(void);
const char *voice_ble_device_name(void);
const char *voice_ble_device_uuid(void);

bool voice_ble_is_connected(void);
bool voice_ble_has_active_link(void);
bool voice_ble_control_ready(void);
bool voice_ble_is_ready(void);
bool voice_ble_ota_is_active(void);

/* Control plane (JSON-RPC responses and events on control_tx, auto-chunked via 0x1F) */
esp_err_t voice_ble_send_control_json(const char *json);
esp_err_t voice_ble_send_event(const char *event_name, uint64_t timestamp_ms, const char *data_json);

/* Stream plane (binary frames on stream_tx) */
esp_err_t voice_ble_send_audio(uint16_t stream_id, uint32_t seq, uint32_t timestamp_ms,
                               uint8_t flags, const uint8_t *payload, size_t len);
esp_err_t voice_ble_send_imu(uint16_t stream_id, uint16_t seq, uint32_t timestamp_ms,
                             const uint8_t *samples, size_t sample_bytes);
esp_err_t voice_ble_send_input(uint16_t stream_id, uint16_t seq, uint32_t timestamp_ms,
                               uint32_t buttons_mask);
esp_err_t voice_ble_send_nop(void);
uint32_t voice_ble_last_tx_ms(void);

/* BLE link parameter management */
esp_err_t voice_ble_request_fast_interval(void);
esp_err_t voice_ble_request_slow_interval(void);
void voice_ble_check_fast_interval(void);
