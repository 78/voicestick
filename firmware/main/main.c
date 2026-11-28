#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button_gpio.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "iot_button.h"

#include "audio_pipeline.h"
#include "board.h"
#include "ui_status.h"
#include "voice_ble.h"

static const char *TAG = "voice_stick";

#define BATTERY_REFRESH_FALLBACK_US (60 * 1000000ULL)
#define ACTIVE_WINDOW_TIMEOUT_MS (30 * 1000)
#define ACTIVE_WINDOW_TIMEOUT_US (ACTIVE_WINDOW_TIMEOUT_MS * 1000ULL)
#define HANDSHAKE_WINDOW_TIMEOUT_US (3 * 1000000ULL)
#define FAST_DOWNLINK_WINDOW_US (1500 * 1000LL)
#define FAST_DOWNLINK_NOP_INTERVAL_MS 180
#define FAST_DOWNLINK_NOP_INTERVAL_US (FAST_DOWNLINK_NOP_INTERVAL_MS * 1000LL)
#define DISPLAY_ACTIVE_BRIGHTNESS 128
#define DISPLAY_DIM_BRIGHTNESS 32
#define DEEP_SLEEP_TIMEOUT_US (CONFIG_VOICE_STICK_DEEP_SLEEP_TIMEOUT_S * 1000000ULL)
#define DISCONNECTED_SLEEP_TIMEOUT_US (CONFIG_VOICE_STICK_DISCONNECTED_SLEEP_TIMEOUT_S * 1000000ULL)
#define MAX_BUTTONS 4
#define IMU_MIN_LEASE_MS 500
#define IMU_MAX_LEASE_MS 5000

typedef enum {
    AUDIO_TRIGGER_IMMEDIATE,
    AUDIO_TRIGGER_BUTTON_HOLD,
    AUDIO_TRIGGER_BUTTON_TOGGLE,
} audio_trigger_mode_t;

typedef enum {
    DISPLAY_SCENE_IDLE,
    DISPLAY_SCENE_RECORDING,
    DISPLAY_SCENE_BUSY,
    DISPLAY_SCENE_ERROR,
} display_scene_t;

typedef enum {
    APP_EVENT_BUTTON_DOWN,
    APP_EVENT_BUTTON_UP,
    APP_EVENT_CONTROL_RPC,
    APP_EVENT_BLE_CONNECTED,
    APP_EVENT_BLE_DISCONNECTED,
    APP_EVENT_PAIRING_OPENED,
    APP_EVENT_PAIRING_CLOSED,
    APP_EVENT_POWER_IRQ,
    APP_EVENT_BATTERY_REFRESH,
    APP_EVENT_INACTIVITY_TIMEOUT,
    APP_EVENT_HANDSHAKE_TIMEOUT,
    APP_EVENT_ENTER_DEEP_SLEEP,
    APP_EVENT_OTA_BEGIN,
    APP_EVENT_OTA_PROGRESS,
    APP_EVENT_OTA_DONE,
    APP_EVENT_OTA_END,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    uint32_t button;
    uint32_t timestamp_ms;
    uint32_t written;
    uint32_t size;
    char *json;
} app_event_t;

/* Owned by app_event_task. */
static const board_button_t *s_buttons;
static size_t s_button_count;
static button_handle_t s_button_handles[MAX_BUTTONS];
static uint32_t s_buttons_mask;
static const board_imu_t *s_imu;

static uint16_t s_next_stream_id = 1;

static struct {
    bool active;
    uint16_t stream_id;
    uint16_t seq;
} s_input_stream;

static struct {
    bool active;
    uint16_t stream_id;
    uint32_t seq;
    bool capturing;
    audio_trigger_mode_t trigger_mode;
    int trigger_button_idx;
    bool local_display_feedback;
} s_audio_stream;

static struct {
    bool active;
    uint16_t stream_id;
    uint16_t seq;
    bool include_gyro;
    uint16_t rate_hz;
    uint32_t lease_ms;
    int64_t sample_period_us;
    int64_t lease_until_us;
    int64_t next_sample_us;
} s_imu_stream;

static display_scene_t s_display_scene = DISPLAY_SCENE_IDLE;
static char s_display_text[96];
static bool s_display_dimmed;
static bool s_handshake_window_open;
static int64_t s_fast_downlink_until_us;
static int64_t s_fast_downlink_next_nop_us;
static bool s_ota_updating;
static bool s_pairing_open;
static bool s_recording_pm_locked;
static bool s_ota_pm_locked;

static bool s_battery_valid;
static int s_battery_percent;
static bool s_battery_charging;
static bool s_usb_powered;
static bool s_battery_reported;
static int s_last_reported_percent;
static bool s_last_reported_charging;
static bool s_last_reported_usb;

static esp_pm_lock_handle_t s_cpu_freq_lock;
#if CONFIG_IDF_TARGET_ESP32C3
static esp_pm_lock_handle_t s_button_no_light_sleep;
/* Each held button owns one reference to the same PM lock. */
static bool s_button_pm_locked[MAX_BUTTONS];
#endif
static esp_timer_handle_t s_inactivity_timer;
static esp_timer_handle_t s_handshake_timer;
static esp_timer_handle_t s_deep_sleep_timer;
static esp_timer_handle_t s_battery_refresh_timer;
static QueueHandle_t s_app_event_queue;

static void update_battery_status(void);
static void update_ble_link_speed(void);
static void render_display_state(void);

static uint64_t now_timestamp_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static bool is_external_powered(void)
{
    return s_battery_charging || s_usb_powered;
}

static uint16_t alloc_stream_id(void)
{
    for (int attempt = 0; attempt < 65535; ++attempt) {
        uint16_t id = s_next_stream_id++;
        if (s_next_stream_id == 0) {
            s_next_stream_id = 1;
        }
        if (id == 0) {
            continue;
        }
        if ((s_input_stream.active && s_input_stream.stream_id == id) ||
            (s_audio_stream.active && s_audio_stream.stream_id == id) ||
            (s_imu_stream.active && s_imu_stream.stream_id == id)) {
            continue;
        }
        return id;
    }
    return 1;
}

static esp_err_t init_power_management(void)
{
    const esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "recording_cpu", &s_cpu_freq_lock);
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_IDF_TARGET_ESP32C3
    return esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "button_held", &s_button_no_light_sleep);
#else
    return ESP_OK;
#endif
}

static esp_err_t acquire_recording_pm_locks(void)
{
    if (s_recording_pm_locked) {
        return ESP_OK;
    }
    esp_err_t err = esp_pm_lock_acquire(s_cpu_freq_lock);
    if (err != ESP_OK) {
        return err;
    }
    s_recording_pm_locked = true;
    return ESP_OK;
}

static void release_recording_pm_locks(void)
{
    if (!s_recording_pm_locked) {
        return;
    }
    (void)esp_pm_lock_release(s_cpu_freq_lock);
    s_recording_pm_locked = false;
}

static esp_err_t acquire_ota_pm_locks(void)
{
    if (s_ota_pm_locked) {
        return ESP_OK;
    }
    esp_err_t err = esp_pm_lock_acquire(s_cpu_freq_lock);
    if (err != ESP_OK) {
        return err;
    }
    s_ota_pm_locked = true;
    return ESP_OK;
}

static void release_ota_pm_locks(void)
{
    if (!s_ota_pm_locked) {
        return;
    }
    (void)esp_pm_lock_release(s_cpu_freq_lock);
    s_ota_pm_locked = false;
}

static void restart_inactivity_timer(void)
{
    if (!s_inactivity_timer) {
        return;
    }
    (void)esp_timer_stop(s_inactivity_timer);
    if (!s_audio_stream.capturing && !s_imu_stream.active && !s_ota_updating) {
        esp_err_t err = esp_timer_start_once(s_inactivity_timer, ACTIVE_WINDOW_TIMEOUT_US);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "start inactivity timer failed: %s", esp_err_to_name(err));
        }
    }
}

static void restart_deep_sleep_timer(void)
{
    if (!s_deep_sleep_timer) {
        return;
    }
    (void)esp_timer_stop(s_deep_sleep_timer);
    if (!s_imu_stream.active && !s_audio_stream.capturing && !s_ota_updating &&
        !is_external_powered()) {
        uint64_t timeout_us = DEEP_SLEEP_TIMEOUT_US;
        if (!voice_ble_is_connected() && DISCONNECTED_SLEEP_TIMEOUT_US < timeout_us) {
            timeout_us = DISCONNECTED_SLEEP_TIMEOUT_US;
        }
        esp_err_t err = esp_timer_start_once(s_deep_sleep_timer, timeout_us);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "start deep sleep timer failed: %s", esp_err_to_name(err));
        }
    }
}

static void start_fast_downlink_window(void)
{
    if (!board_has_display() || !voice_ble_is_connected() || s_handshake_window_open ||
        s_ota_updating || voice_ble_ota_is_active()) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    s_fast_downlink_until_us = now + FAST_DOWNLINK_WINDOW_US;
    s_fast_downlink_next_nop_us = now + FAST_DOWNLINK_NOP_INTERVAL_US;
}

static void stop_fast_downlink_window(void)
{
    s_fast_downlink_until_us = 0;
    s_fast_downlink_next_nop_us = 0;
}

static void update_ble_link_speed(void)
{
    if (!voice_ble_is_connected()) {
        return;
    }
    if (s_ota_updating || voice_ble_ota_is_active()) {
        (void)voice_ble_request_fast_interval();
    } else if (!s_handshake_window_open) {
        (void)voice_ble_request_slow_interval();
    }
}

static void note_activity(void)
{
    if (s_display_dimmed) {
        esp_err_t err = ui_status_set_brightness(DISPLAY_ACTIVE_BRIGHTNESS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restore brightness failed: %s", esp_err_to_name(err));
        } else {
            s_display_dimmed = false;
            ui_status_set_idle_dimmed(false);
        }
    }
    restart_inactivity_timer();
    restart_deep_sleep_timer();
    update_ble_link_speed();
}

static void render_display_state(void)
{
    if (s_ota_updating || s_pairing_open) {
        return;
    }
    if (!voice_ble_is_connected()) {
        ui_status_set_advertising();
        return;
    }
    if (s_audio_stream.capturing && s_audio_stream.local_display_feedback) {
        ui_status_set_recording(s_audio_stream.stream_id);
        return;
    }
    switch (s_display_scene) {
    case DISPLAY_SCENE_IDLE:
        ui_status_set_idle_hint(s_display_text);
        ui_status_set_idle();
        break;
    case DISPLAY_SCENE_RECORDING:
        ui_status_set_recording(0);
        break;
    case DISPLAY_SCENE_BUSY:
        ui_status_set_partial_text(s_display_text);
        break;
    case DISPLAY_SCENE_ERROR:
        ui_status_set_error(s_display_text[0] ? s_display_text : "Error");
        break;
    }
}

static void emit_stream_closed(uint16_t stream_id, const char *reason)
{
    char data[96];
    snprintf(data, sizeof(data), "{\"stream_id\":%u,\"reason\":\"%s\"}", stream_id, reason);
    (void)voice_ble_send_event("stream.closed", now_timestamp_ms(), data);
}

static bool start_audio_capture(void)
{
    if (!s_audio_stream.active || s_audio_stream.capturing ||
        s_ota_updating || voice_ble_ota_is_active() || !voice_ble_is_connected()) {
        return false;
    }
    esp_err_t err = acquire_recording_pm_locks();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "acquire recording pm lock failed: %s", esp_err_to_name(err));
        return false;
    }
    err = audio_pipeline_start(s_audio_stream.stream_id, s_audio_stream.seq);
    if (err != ESP_OK) {
        release_recording_pm_locks();
        ESP_LOGE(TAG, "audio pipeline start failed: %s", esp_err_to_name(err));
        return false;
    }
    s_audio_stream.capturing = true;
    stop_fast_downlink_window();
    note_activity();
    if (s_audio_stream.local_display_feedback) {
        ui_status_set_recording(s_audio_stream.stream_id);
    }
    return true;
}

static void stop_audio_capture(bool wait_for_exit)
{
    if (!s_audio_stream.capturing) {
        return;
    }
    s_audio_stream.capturing = false;
    (void)audio_pipeline_stop(wait_for_exit);
    s_audio_stream.seq = audio_pipeline_next_seq();
    release_recording_pm_locks();
    start_fast_downlink_window();
    restart_inactivity_timer();
    restart_deep_sleep_timer();
    update_ble_link_speed();
    if (s_audio_stream.local_display_feedback) {
        render_display_state();
    }
}

static void close_imu_stream(bool emit_closed, const char *reason)
{
    if (!s_imu_stream.active) {
        return;
    }
    const uint16_t closed_id = s_imu_stream.stream_id;
    s_imu_stream.active = false;
    s_imu_stream.lease_until_us = 0;
    if (s_imu) {
        (void)s_imu->set_enabled(false, false, 0);
    }
    if (emit_closed && reason) {
        emit_stream_closed(closed_id, reason);
    }
    restart_inactivity_timer();
    restart_deep_sleep_timer();
    update_ble_link_speed();
}

static void close_all_streams(bool emit_closed, const char *reason)
{
    if (s_audio_stream.active) {
        const uint16_t id = s_audio_stream.stream_id;
        stop_audio_capture(true);
        s_audio_stream.active = false;
        if (emit_closed && reason) {
            emit_stream_closed(id, reason);
        }
    }
    if (s_imu_stream.active) {
        close_imu_stream(emit_closed, reason);
    }
    if (s_input_stream.active) {
        const uint16_t id = s_input_stream.stream_id;
        s_input_stream.active = false;
        if (emit_closed && reason) {
            emit_stream_closed(id, reason);
        }
    }
}

static void reset_runtime_state(bool emit_closed, const char *reason)
{
    close_all_streams(emit_closed, reason);
    stop_fast_downlink_window();
    s_next_stream_id = 1;
    s_display_scene = DISPLAY_SCENE_IDLE;
    s_display_text[0] = '\0';
    render_display_state();
    restart_inactivity_timer();
    restart_deep_sleep_timer();
    update_ble_link_speed();
}

static void send_rpc_json(cJSON *root)
{
    char *printed = cJSON_PrintUnformatted(root);
    if (printed) {
        (void)voice_ble_send_control_json(printed);
        cJSON_free(printed);
    }
}

static void send_rpc_result(bool has_id, double id_num, cJSON *result)
{
    if (!has_id) {
        cJSON_Delete(result);
        return;
    }
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        cJSON_Delete(result);
        return;
    }
    cJSON_AddNumberToObject(resp, "id", id_num);
    cJSON_AddItemToObject(resp, "result", result ? result : cJSON_CreateObject());
    send_rpc_json(resp);
    cJSON_Delete(resp);
}

static void send_rpc_error(bool has_id, double id_num, const char *code, const char *message)
{
    if (!has_id) {
        return;
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON *err = cJSON_CreateObject();
    if (!resp || !err) {
        cJSON_Delete(resp);
        cJSON_Delete(err);
        return;
    }
    cJSON_AddNumberToObject(resp, "id", id_num);
    cJSON_AddStringToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(resp, "error", err);
    send_rpc_json(resp);
    cJSON_Delete(resp);
}

static bool json_exact_uint(const cJSON *item, uint64_t *out)
{
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    const double v = item->valuedouble;
    if (!(v >= 0.0 && v <= 9007199254740991.0) || floor(v) != v) {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

static int find_button_index(const char *name)
{
    if (!name) {
        return -1;
    }
    for (size_t i = 0; i < s_button_count; ++i) {
        if (strcmp(s_buttons[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void handle_rpc_device_get_info(bool has_id, double id_num)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *version = app_desc ? app_desc->version : "unknown";
    const char *uuid = voice_ble_device_uuid();

    cJSON *res = cJSON_CreateObject();
    cJSON_AddNumberToObject(res, "protocol_version", VOICE_BLE_PROTOCOL_VERSION);
    cJSON_AddStringToObject(res, "hardware", board_hardware_name());
    cJSON_AddStringToObject(res, "firmware_version", version);
    if (uuid && uuid[0] != '\0') {
        cJSON_AddStringToObject(res, "device_uuid", uuid);
    }
    cJSON_AddNumberToObject(res, "max_message_bytes", VOICE_BLE_MAX_CONTROL_MESSAGE_BYTES);

    cJSON *buttons = cJSON_AddArrayToObject(res, "buttons");
    for (size_t i = 0; i < s_button_count; ++i) {
        cJSON_AddItemToArray(buttons, cJSON_CreateString(s_buttons[i].name));
    }

    cJSON *streams = cJSON_AddObjectToObject(res, "streams");
    cJSON_AddObjectToObject(streams, "input");

    cJSON *audio_in = cJSON_AddObjectToObject(streams, "audio_in");
    cJSON *codecs = cJSON_AddArrayToObject(audio_in, "codecs");
    cJSON_AddItemToArray(codecs, cJSON_CreateString("opus"));
    cJSON *rates = cJSON_AddArrayToObject(audio_in, "sample_rates");
    cJSON_AddItemToArray(rates, cJSON_CreateNumber(16000));
    cJSON *channels = cJSON_AddArrayToObject(audio_in, "channels");
    cJSON_AddItemToArray(channels, cJSON_CreateNumber(1));
    cJSON *frames = cJSON_AddArrayToObject(audio_in, "frame_ms");
    cJSON_AddItemToArray(frames, cJSON_CreateNumber(60));
    cJSON *triggers = cJSON_AddArrayToObject(audio_in, "triggers");
    cJSON_AddItemToArray(triggers, cJSON_CreateString("button_hold"));
    cJSON_AddItemToArray(triggers, cJSON_CreateString("button_toggle"));
    cJSON_AddItemToArray(triggers, cJSON_CreateString("immediate"));

    if (s_imu) {
        cJSON *imu = cJSON_AddObjectToObject(streams, "imu");
        cJSON *imu_ch = cJSON_AddArrayToObject(imu, "channels");
        cJSON_AddItemToArray(imu_ch, cJSON_CreateString("accel"));
        if (s_imu->has_gyro) {
            cJSON_AddItemToArray(imu_ch, cJSON_CreateString("gyro"));
        }
        cJSON *imu_rates = cJSON_AddArrayToObject(imu, "rates_hz");
        cJSON_AddItemToArray(imu_rates, cJSON_CreateNumber(50));
        if (s_imu->max_rate_hz >= 100) {
            cJSON_AddItemToArray(imu_rates, cJSON_CreateNumber(100));
        }
        cJSON_AddNumberToObject(imu, "accel_lsb_per_g", s_imu->accel_lsb_per_g);
        if (s_imu->has_gyro) {
            cJSON_AddNumberToObject(imu, "gyro_lsb_per_dps", s_imu->gyro_lsb_per_dps);
        }
        cJSON_AddNumberToObject(imu, "max_lease_ms", IMU_MAX_LEASE_MS);
    }

    if (board_has_display()) {
        cJSON *display = cJSON_AddObjectToObject(res, "display");
        cJSON *scenes = cJSON_AddArrayToObject(display, "scenes");
        cJSON_AddItemToArray(scenes, cJSON_CreateString("idle"));
        cJSON_AddItemToArray(scenes, cJSON_CreateString("recording"));
        cJSON_AddItemToArray(scenes, cJSON_CreateString("busy"));
        cJSON_AddItemToArray(scenes, cJSON_CreateString("error"));
    } else {
        cJSON_AddNullToObject(res, "display");
    }

    if (s_battery_valid) {
        cJSON *bat = cJSON_AddObjectToObject(res, "battery");
        cJSON_AddNumberToObject(bat, "percent", s_battery_percent);
        cJSON_AddBoolToObject(bat, "charging", s_battery_charging);
        cJSON_AddBoolToObject(bat, "usb_powered", s_usb_powered);
        s_battery_reported = true;
        s_last_reported_percent = s_battery_percent;
        s_last_reported_charging = s_battery_charging;
        s_last_reported_usb = s_usb_powered;
    } else {
        cJSON_AddNullToObject(res, "battery");
    }

    send_rpc_result(has_id, id_num, res);
}

static void handle_rpc_stream_start(bool has_id, double id_num, const cJSON *params)
{
    if (s_ota_updating || voice_ble_ota_is_active()) {
        send_rpc_error(has_id, id_num, "busy", "ota active");
        return;
    }
    if (!cJSON_IsObject(params)) {
        send_rpc_error(has_id, id_num, "invalid_params", "params object required");
        return;
    }
    const cJSON *kind_item = cJSON_GetObjectItemCaseSensitive(params, "kind");
    if (!cJSON_IsString(kind_item)) {
        send_rpc_error(has_id, id_num, "invalid_params", "kind string required");
        return;
    }
    const char *kind = kind_item->valuestring;

    if (strcmp(kind, "input") == 0) {
        if (s_input_stream.active) {
            send_rpc_error(has_id, id_num, "busy", "input stream already active");
            return;
        }
        const uint16_t id = alloc_stream_id();
        s_input_stream.active = true;
        s_input_stream.stream_id = id;
        s_input_stream.seq = 0;

        cJSON *res = cJSON_CreateObject();
        cJSON_AddNumberToObject(res, "stream_id", id);
        cJSON_AddNumberToObject(res, "buttons_mask", s_buttons_mask);
        send_rpc_result(has_id, id_num, res);
        ESP_LOGI(TAG, "stream.start input id=%u mask=0x%" PRIx32, id, s_buttons_mask);
        return;
    }

    if (strcmp(kind, "audio_in") == 0) {
        if (s_audio_stream.active) {
            send_rpc_error(has_id, id_num, "busy", "audio_in stream already active");
            return;
        }
        const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(params, "format");
        if (fmt != NULL) {
            if (!cJSON_IsObject(fmt)) {
                send_rpc_error(has_id, id_num, "invalid_params", "format must be an object");
                return;
            }
            const cJSON *codec = cJSON_GetObjectItemCaseSensitive(fmt, "codec");
            if (codec && (!cJSON_IsString(codec) || strcmp(codec->valuestring, "opus") != 0)) {
                send_rpc_error(has_id, id_num, "unsupported", "unsupported audio codec");
                return;
            }
            uint64_t val = 0;
            const cJSON *sr = cJSON_GetObjectItemCaseSensitive(fmt, "sample_rate");
            if (sr && (!json_exact_uint(sr, &val) || val != 16000)) {
                send_rpc_error(has_id, id_num, "unsupported", "unsupported sample_rate");
                return;
            }
            const cJSON *ch = cJSON_GetObjectItemCaseSensitive(fmt, "channels");
            if (ch && (!json_exact_uint(ch, &val) || val != 1)) {
                send_rpc_error(has_id, id_num, "unsupported", "unsupported channels");
                return;
            }
            const cJSON *fm = cJSON_GetObjectItemCaseSensitive(fmt, "frame_ms");
            if (fm && (!json_exact_uint(fm, &val) || val != 60)) {
                send_rpc_error(has_id, id_num, "unsupported", "unsupported frame_ms");
                return;
            }
        }

        audio_trigger_mode_t trig_mode = AUDIO_TRIGGER_IMMEDIATE;
        int trig_btn = -1;
        const cJSON *trig = cJSON_GetObjectItemCaseSensitive(params, "trigger");
        if (trig != NULL && !cJSON_IsNull(trig)) {
            if (!cJSON_IsObject(trig)) {
                send_rpc_error(has_id, id_num, "invalid_params", "trigger must be an object");
                return;
            }
            const cJSON *mode = cJSON_GetObjectItemCaseSensitive(trig, "mode");
            if (!cJSON_IsString(mode)) {
                send_rpc_error(has_id, id_num, "invalid_params", "trigger.mode required");
                return;
            }
            if (strcmp(mode->valuestring, "immediate") == 0) {
                trig_mode = AUDIO_TRIGGER_IMMEDIATE;
            } else if (strcmp(mode->valuestring, "button_hold") == 0 ||
                       strcmp(mode->valuestring, "button_toggle") == 0) {
                trig_mode = strcmp(mode->valuestring, "button_hold") == 0
                    ? AUDIO_TRIGGER_BUTTON_HOLD : AUDIO_TRIGGER_BUTTON_TOGGLE;
                const cJSON *btn = cJSON_GetObjectItemCaseSensitive(trig, "button");
                if (!cJSON_IsString(btn)) {
                    send_rpc_error(has_id, id_num, "invalid_params", "trigger.button required");
                    return;
                }
                trig_btn = find_button_index(btn->valuestring);
                if (trig_btn < 0) {
                    send_rpc_error(has_id, id_num, "invalid_params", "unknown trigger button");
                    return;
                }
            } else {
                send_rpc_error(has_id, id_num, "unsupported", "unsupported trigger mode");
                return;
            }
        }

        bool local_feedback = false;
        const cJSON *fb = cJSON_GetObjectItemCaseSensitive(params, "local_display_feedback");
        if (fb != NULL) {
            if (!cJSON_IsBool(fb)) {
                send_rpc_error(has_id, id_num, "invalid_params", "local_display_feedback must be bool");
                return;
            }
            local_feedback = cJSON_IsTrue(fb);
        }

        const uint16_t id = alloc_stream_id();
        s_audio_stream.active = true;
        s_audio_stream.stream_id = id;
        s_audio_stream.seq = 0;
        s_audio_stream.capturing = false;
        s_audio_stream.trigger_mode = trig_mode;
        s_audio_stream.trigger_button_idx = trig_btn;
        s_audio_stream.local_display_feedback = local_feedback;

        if (trig_mode == AUDIO_TRIGGER_IMMEDIATE && !start_audio_capture()) {
            s_audio_stream.active = false;
            send_rpc_error(has_id, id_num, "internal", "failed to start audio capture");
            return;
        }

        cJSON *res = cJSON_CreateObject();
        cJSON_AddNumberToObject(res, "stream_id", id);
        send_rpc_result(has_id, id_num, res);
        ESP_LOGI(TAG, "stream.start audio_in id=%u trigger=%d btn=%d", id, trig_mode, trig_btn);
        return;
    }

    if (strcmp(kind, "imu") == 0) {
        if (!s_imu) {
            send_rpc_error(has_id, id_num, "unsupported", "board has no imu");
            return;
        }
        if (s_imu_stream.active) {
            send_rpc_error(has_id, id_num, "busy", "imu stream already active");
            return;
        }

        const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(params, "format");
        if (!cJSON_IsObject(fmt)) {
            send_rpc_error(has_id, id_num, "invalid_params", "imu format object required");
            return;
        }
        const cJSON *ch_arr = cJSON_GetObjectItemCaseSensitive(fmt, "channels");
        if (!cJSON_IsArray(ch_arr) || cJSON_GetArraySize(ch_arr) == 0) {
            send_rpc_error(has_id, id_num, "invalid_params", "format.channels array required");
            return;
        }
        bool want_accel = false;
        bool want_gyro = false;
        const int ch_count = cJSON_GetArraySize(ch_arr);
        for (int i = 0; i < ch_count; ++i) {
            const cJSON *item = cJSON_GetArrayItem(ch_arr, i);
            if (!cJSON_IsString(item)) {
                send_rpc_error(has_id, id_num, "invalid_params", "channel must be string");
                return;
            }
            if (strcmp(item->valuestring, "accel") == 0) {
                want_accel = true;
            } else if (strcmp(item->valuestring, "gyro") == 0) {
                want_gyro = true;
            } else {
                send_rpc_error(has_id, id_num, "unsupported", "unsupported imu channel");
                return;
            }
        }
        if (!want_accel) {
            send_rpc_error(has_id, id_num, "unsupported", "accel channel required");
            return;
        }
        if (want_gyro && !s_imu->has_gyro) {
            send_rpc_error(has_id, id_num, "unsupported", "board imu has no gyro");
            return;
        }

        uint64_t rate_hz = 0;
        if (!json_exact_uint(cJSON_GetObjectItemCaseSensitive(fmt, "rate_hz"), &rate_hz) ||
            (rate_hz != 50 && rate_hz != 100) || rate_hz > s_imu->max_rate_hz) {
            send_rpc_error(has_id, id_num, "unsupported", "unsupported imu rate_hz");
            return;
        }

        uint64_t lease_ms = 0;
        if (!json_exact_uint(cJSON_GetObjectItemCaseSensitive(params, "lease_ms"), &lease_ms) ||
            lease_ms < IMU_MIN_LEASE_MS || lease_ms > IMU_MAX_LEASE_MS) {
            send_rpc_error(has_id, id_num, "invalid_params", "lease_ms must be 500..5000");
            return;
        }

        if (!s_imu->set_enabled(true, want_gyro, (uint16_t)rate_hz)) {
            (void)s_imu->set_enabled(false, false, 0);
            send_rpc_error(has_id, id_num, "internal", "failed to enable imu");
            return;
        }

        const uint16_t id = alloc_stream_id();
        const int64_t now = esp_timer_get_time();
        s_imu_stream.active = true;
        s_imu_stream.stream_id = id;
        s_imu_stream.seq = 0;
        s_imu_stream.include_gyro = want_gyro;
        s_imu_stream.rate_hz = (uint16_t)rate_hz;
        s_imu_stream.lease_ms = (uint32_t)lease_ms;
        s_imu_stream.sample_period_us = 1000000LL / (int64_t)rate_hz;
        s_imu_stream.lease_until_us = now + (int64_t)lease_ms * 1000LL;
        s_imu_stream.next_sample_us = now + (want_gyro ? 80000LL : 20000LL);
        stop_fast_downlink_window();

        note_activity();
        cJSON *res = cJSON_CreateObject();
        cJSON_AddNumberToObject(res, "stream_id", id);
        send_rpc_result(has_id, id_num, res);
        ESP_LOGI(TAG, "stream.start imu id=%u gyro=%d rate=%u lease=%" PRIu32,
                 id, want_gyro, (unsigned)rate_hz, s_imu_stream.lease_ms);
        return;
    }

    send_rpc_error(has_id, id_num, "unsupported", "unsupported stream kind");
}

static void handle_rpc_stream_renew(bool has_id, double id_num, const cJSON *params)
{
    uint64_t sid = 0;
    if (!cJSON_IsObject(params) ||
        !json_exact_uint(cJSON_GetObjectItemCaseSensitive(params, "stream_id"), &sid) ||
        sid == 0 || sid > UINT16_MAX) {
        send_rpc_error(has_id, id_num, "invalid_params", "valid stream_id required");
        return;
    }
    if (s_imu_stream.active && s_imu_stream.stream_id == (uint16_t)sid) {
        const int64_t now = esp_timer_get_time();
        s_imu_stream.lease_until_us = now + (int64_t)s_imu_stream.lease_ms * 1000LL;
        send_rpc_result(has_id, id_num, cJSON_CreateObject());
        return;
    }
    send_rpc_error(has_id, id_num, "not_found", "stream not found or not leased");
}

static void handle_rpc_stream_stop(bool has_id, double id_num, const cJSON *params)
{
    uint64_t sid = 0;
    if (!cJSON_IsObject(params) ||
        !json_exact_uint(cJSON_GetObjectItemCaseSensitive(params, "stream_id"), &sid) ||
        sid == 0 || sid > UINT16_MAX) {
        send_rpc_error(has_id, id_num, "invalid_params", "valid stream_id required");
        return;
    }
    const uint16_t id = (uint16_t)sid;
    if (s_input_stream.active && s_input_stream.stream_id == id) {
        s_input_stream.active = false;
        ESP_LOGI(TAG, "stream.stop input id=%u", id);
        send_rpc_result(has_id, id_num, cJSON_CreateObject());
        return;
    }
    if (s_audio_stream.active && s_audio_stream.stream_id == id) {
        stop_audio_capture(true);
        s_audio_stream.active = false;
        ESP_LOGI(TAG, "stream.stop audio_in id=%u", id);
        send_rpc_result(has_id, id_num, cJSON_CreateObject());
        return;
    }
    if (s_imu_stream.active && s_imu_stream.stream_id == id) {
        close_imu_stream(false, NULL);
        ESP_LOGI(TAG, "stream.stop imu id=%u", id);
        send_rpc_result(has_id, id_num, cJSON_CreateObject());
        return;
    }
    send_rpc_error(has_id, id_num, "not_found", "stream not found");
}

static void handle_rpc_display_set(bool has_id, double id_num, const cJSON *params)
{
    if (!board_has_display()) {
        send_rpc_error(has_id, id_num, "unsupported", "board has no display");
        return;
    }
    if (!cJSON_IsObject(params)) {
        send_rpc_error(has_id, id_num, "invalid_params", "params object required");
        return;
    }
    const cJSON *scene_item = cJSON_GetObjectItemCaseSensitive(params, "scene");
    if (!cJSON_IsString(scene_item)) {
        send_rpc_error(has_id, id_num, "invalid_params", "scene string required");
        return;
    }
    display_scene_t scene;
    if (strcmp(scene_item->valuestring, "idle") == 0) {
        scene = DISPLAY_SCENE_IDLE;
    } else if (strcmp(scene_item->valuestring, "recording") == 0) {
        scene = DISPLAY_SCENE_RECORDING;
    } else if (strcmp(scene_item->valuestring, "busy") == 0) {
        scene = DISPLAY_SCENE_BUSY;
    } else if (strcmp(scene_item->valuestring, "error") == 0) {
        scene = DISPLAY_SCENE_ERROR;
    } else {
        send_rpc_error(has_id, id_num, "invalid_params", "unknown display scene");
        return;
    }

    const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(params, "text");
    if (text_item != NULL && !cJSON_IsString(text_item)) {
        send_rpc_error(has_id, id_num, "invalid_params", "text must be string");
        return;
    }

    s_display_scene = scene;
    strlcpy(s_display_text, text_item ? text_item->valuestring : "", sizeof(s_display_text));
    if (scene == DISPLAY_SCENE_IDLE || scene == DISPLAY_SCENE_ERROR) {
        stop_fast_downlink_window();
    }
    note_activity();
    render_display_state();
    send_rpc_result(has_id, id_num, cJSON_CreateObject());
}

static void handle_control_rpc(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }

    bool has_id = false;
    double id_num = 0;
    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    uint64_t parsed_id = 0;
    if (id_item != NULL) {
        if (!json_exact_uint(id_item, &parsed_id) || parsed_id == 0) {
            cJSON_Delete(root);
            return;
        }
        has_id = true;
        id_num = (double)parsed_id;
    }

    const cJSON *method_item = cJSON_GetObjectItemCaseSensitive(root, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (!cJSON_IsString(method_item)) {
        send_rpc_error(has_id, id_num, "invalid_params", "method string required");
        cJSON_Delete(root);
        return;
    }
    const char *method = method_item->valuestring;

    /* Renewals keep IMU alive without waking the display if already dimmed. */
    if (strcmp(method, "stream.renew") != 0) {
        note_activity();
    }

    if (strcmp(method, "device.get_info") == 0) {
        handle_rpc_device_get_info(has_id, id_num);
    } else if (strcmp(method, "device.reset") == 0) {
        reset_runtime_state(false, NULL);
        send_rpc_result(has_id, id_num, cJSON_CreateObject());
        ESP_LOGI(TAG, "device.reset completed");
    } else if (strcmp(method, "stream.start") == 0) {
        handle_rpc_stream_start(has_id, id_num, params);
    } else if (strcmp(method, "stream.renew") == 0) {
        handle_rpc_stream_renew(has_id, id_num, params);
    } else if (strcmp(method, "stream.stop") == 0) {
        handle_rpc_stream_stop(has_id, id_num, params);
    } else if (strcmp(method, "display.set") == 0) {
        handle_rpc_display_set(has_id, id_num, params);
    } else {
        send_rpc_error(has_id, id_num, "unsupported", "unknown method");
    }

    cJSON_Delete(root);
}

static void handle_button_event(const app_event_t *event)
{
    const uint32_t idx = event->button;
    if (idx >= s_button_count) {
        return;
    }
    const bool down = event->type == APP_EVENT_BUTTON_DOWN;
    const uint32_t bit = 1u << idx;
    if (down) {
        s_buttons_mask |= bit;
    } else {
        s_buttons_mask &= ~bit;
    }

    ESP_LOGI(TAG, "button %s %s mask=0x%" PRIx32,
             s_buttons[idx].name, down ? "down" : "up", s_buttons_mask);
    note_activity();

    if (!voice_ble_has_active_link()) {
        if (idx == 0 && down && !s_audio_stream.capturing && !s_ota_updating) {
            voice_ble_open_pairing();
        }
        return;
    }

    /* Send HID input snapshot first so it strictly precedes any audio START/END on stream_tx. */
    if (s_input_stream.active) {
        (void)voice_ble_send_input(s_input_stream.stream_id, s_input_stream.seq++,
                                   event->timestamp_ms, s_buttons_mask);
    }

    bool started_audio = false;
    if (s_audio_stream.active && !s_ota_updating &&
        s_audio_stream.trigger_button_idx == (int)idx) {
        if (s_audio_stream.trigger_mode == AUDIO_TRIGGER_BUTTON_HOLD) {
            if (down && !s_audio_stream.capturing) {
                started_audio = start_audio_capture();
            } else if (!down && s_audio_stream.capturing) {
                stop_audio_capture(false);
            }
        } else if (s_audio_stream.trigger_mode == AUDIO_TRIGGER_BUTTON_TOGGLE && down) {
            if (s_audio_stream.capturing) {
                stop_audio_capture(false);
            } else {
                started_audio = start_audio_capture();
            }
        }
    }
    if (!started_audio && !s_audio_stream.capturing) {
        start_fast_downlink_window();
    }
}

static void enter_deep_sleep(bool low_battery)
{
    if (s_ota_updating || voice_ble_ota_is_active() ||
        ((s_imu_stream.active || s_pairing_open) && !low_battery)) {
        restart_deep_sleep_timer();
        return;
    }
    if (s_audio_stream.capturing && !low_battery) {
        restart_deep_sleep_timer();
        return;
    }
    if (low_battery) {
        close_all_streams(true, "low_battery");
    }

    if (is_external_powered()) {
        ESP_LOGI(TAG, "skip deep sleep while charging or USB powered");
        restart_deep_sleep_timer();
        return;
    }

    bool charging = false;
    bool usb_powered = false;
    esp_err_t power_err = board_battery_charging(&charging);
    if (power_err == ESP_OK) {
        power_err = board_usb_powered(&usb_powered);
    }
    if (power_err == ESP_OK && (charging || usb_powered)) {
        s_battery_charging = charging;
        s_usb_powered = usb_powered;
        ESP_LOGI(TAG, "skip deep sleep after fresh power check charging=%d usb=%d",
                 charging, usb_powered);
        restart_deep_sleep_timer();
        return;
    }

    if (!low_battery && gpio_get_level(s_buttons[0].gpio) == 0) {
        ESP_LOGI(TAG, "skip deep sleep: talk button is pressed");
        restart_deep_sleep_timer();
        return;
    }

    ESP_LOGI(TAG, "entering deep sleep low_battery=%d", low_battery);
    release_recording_pm_locks();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_status_set_brightness(0));
    ui_status_prepare_deep_sleep();
    esp_err_t err = board_prepare_deep_sleep(low_battery);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep aborted: %s", esp_err_to_name(err));
        restart_deep_sleep_timer();
        return;
    }

    ESP_LOGI(TAG, "deep sleep go");
    esp_deep_sleep_start();
}

static void queue_app_event(app_event_type_t type)
{
    if (s_app_event_queue) {
        app_event_t event = {.type = type};
        (void)xQueueSend(s_app_event_queue, &event, 0);
    }
}

static void queue_app_event_with_ota(app_event_type_t type, uint32_t written, uint32_t size)
{
    if (s_app_event_queue) {
        app_event_t event = {.type = type, .written = written, .size = size};
        (void)xQueueSend(s_app_event_queue, &event, 0);
    }
}

static void queue_app_event_from_isr(app_event_type_t type, BaseType_t *high_task_woken)
{
    if (s_app_event_queue) {
        app_event_t event = {.type = type};
        (void)xQueueSendFromISR(s_app_event_queue, &event, high_task_woken);
    }
}

static void queue_button_event(app_event_type_t type, uint32_t button)
{
    if (s_app_event_queue) {
        app_event_t event = {
            .type = type,
            .button = button,
            .timestamp_ms = (uint32_t)now_timestamp_ms(),
        };
        (void)xQueueSend(s_app_event_queue, &event, 0);
    }
}

static void button_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    const uint32_t button = (uint32_t)(uintptr_t)usr_data;
#if CONFIG_IDF_TARGET_ESP32C3
    if (!s_button_pm_locked[button]) {
        esp_err_t err = esp_pm_lock_acquire(s_button_no_light_sleep);
        s_button_pm_locked[button] = err == ESP_OK;
    }
#endif
    queue_button_event(APP_EVENT_BUTTON_DOWN, button);
}

static void button_up_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    const uint32_t button = (uint32_t)(uintptr_t)usr_data;
#if CONFIG_IDF_TARGET_ESP32C3
    if (s_button_pm_locked[button]) {
        esp_err_t err = esp_pm_lock_release(s_button_no_light_sleep);
        s_button_pm_locked[button] = err != ESP_OK;
    }
#endif
    queue_button_event(APP_EVENT_BUTTON_UP, button);
}

static void ble_connection_cb(bool connected)
{
    queue_app_event(connected ? APP_EVENT_BLE_CONNECTED : APP_EVENT_BLE_DISCONNECTED);
}

static void ble_pairing_cb(bool open)
{
    queue_app_event(open ? APP_EVENT_PAIRING_OPENED : APP_EVENT_PAIRING_CLOSED);
}

static void ble_control_cb(const char *json)
{
    if (!s_app_event_queue || !json) {
        return;
    }
    char *copy = strdup(json);
    if (!copy) {
        ESP_LOGW(TAG, "drop control rpc: out of memory");
        return;
    }
    app_event_t event = {
        .type = APP_EVENT_CONTROL_RPC,
        .json = copy,
    };
    if (xQueueSend(s_app_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop control rpc: event queue full");
        free(copy);
    }
}

static void ble_ota_cb(voice_ble_ota_event_t event, uint32_t written, uint32_t size)
{
    switch (event) {
    case VOICE_BLE_OTA_EVENT_BEGIN:
        queue_app_event_with_ota(APP_EVENT_OTA_BEGIN, written, size);
        break;
    case VOICE_BLE_OTA_EVENT_PROGRESS:
        queue_app_event_with_ota(APP_EVENT_OTA_PROGRESS, written, size);
        break;
    case VOICE_BLE_OTA_EVENT_DONE:
        queue_app_event_with_ota(APP_EVENT_OTA_DONE, written, size);
        break;
    case VOICE_BLE_OTA_EVENT_ERROR:
    case VOICE_BLE_OTA_EVENT_ABORT:
        queue_app_event(APP_EVENT_OTA_END);
        break;
    }
}

static void app_event_task(void *arg)
{
    (void)arg;
    app_event_t event;
    while (true) {
        TickType_t wait = portMAX_DELAY;
        if (s_imu_stream.active) {
            const int64_t now = esp_timer_get_time();
            if (now >= s_imu_stream.lease_until_us || !voice_ble_is_connected()) {
                ESP_LOGI(TAG, "imu stream id=%u lease expired", s_imu_stream.stream_id);
                close_imu_stream(voice_ble_is_connected(), "lease_expired");
            } else {
                if (now >= s_imu_stream.next_sample_us) {
                    uint8_t samples[12];
                    const size_t sample_bytes = s_imu_stream.include_gyro ? 12 : 6;
                    if (s_imu->read_samples(s_imu_stream.include_gyro, samples)) {
                        (void)voice_ble_send_imu(s_imu_stream.stream_id, s_imu_stream.seq++,
                                                 (uint32_t)((uint64_t)now / 1000ULL),
                                                 samples, sample_bytes);
                    }
                    s_imu_stream.next_sample_us = now + s_imu_stream.sample_period_us;
                }
                const int64_t remaining = s_imu_stream.next_sample_us - esp_timer_get_time();
                wait = remaining > 0 ? pdMS_TO_TICKS((remaining + 999) / 1000) : 0;
            }
        }
        if (s_fast_downlink_until_us > 0) {
            const int64_t now = esp_timer_get_time();
            if (now >= s_fast_downlink_until_us || !voice_ble_is_connected() ||
                s_audio_stream.capturing || s_imu_stream.active ||
                s_ota_updating || voice_ble_ota_is_active()) {
                stop_fast_downlink_window();
            } else {
                if (now >= s_fast_downlink_next_nop_us) {
                    const uint32_t now_ms = (uint32_t)((uint64_t)now / 1000ULL);
                    const uint32_t since_tx_ms = now_ms - voice_ble_last_tx_ms();
                    if (since_tx_ms >= FAST_DOWNLINK_NOP_INTERVAL_MS) {
                        (void)voice_ble_send_nop();
                        s_fast_downlink_next_nop_us = now + FAST_DOWNLINK_NOP_INTERVAL_US;
                    } else {
                        const int64_t defer_us =
                            (int64_t)(FAST_DOWNLINK_NOP_INTERVAL_MS - since_tx_ms) * 1000LL;
                        s_fast_downlink_next_nop_us = now + defer_us;
                    }
                }
                const int64_t remaining = s_fast_downlink_next_nop_us - esp_timer_get_time();
                TickType_t nop_wait = remaining > 0 ? pdMS_TO_TICKS((remaining + 999) / 1000) : 0;
                if (nop_wait == 0 && remaining > 0) {
                    nop_wait = 1;
                }
                if (nop_wait < wait) {
                    wait = nop_wait;
                }
            }
        }

        if (xQueueReceive(s_app_event_queue, &event, wait) != pdTRUE) {
            continue;
        }

        switch (event.type) {
        case APP_EVENT_BUTTON_DOWN:
        case APP_EVENT_BUTTON_UP:
            handle_button_event(&event);
            break;
        case APP_EVENT_CONTROL_RPC:
            handle_control_rpc(event.json);
            free(event.json);
            break;
        case APP_EVENT_BLE_CONNECTED:
            s_battery_reported = false;
            s_handshake_window_open = true;
            if (s_handshake_timer) {
                (void)esp_timer_stop(s_handshake_timer);
                (void)esp_timer_start_once(s_handshake_timer, HANDSHAKE_WINDOW_TIMEOUT_US);
            }
            reset_runtime_state(false, NULL);
            note_activity();
            break;
        case APP_EVENT_BLE_DISCONNECTED:
            s_ota_updating = false;
            s_battery_reported = false;
            s_handshake_window_open = false;
            if (s_handshake_timer) {
                (void)esp_timer_stop(s_handshake_timer);
            }
            release_ota_pm_locks();
            reset_runtime_state(false, NULL);
            if (!s_pairing_open) {
                ui_status_set_advertising();
            }
            break;
        case APP_EVENT_PAIRING_OPENED:
            s_pairing_open = true;
            reset_runtime_state(false, NULL);
            note_activity();
            ui_status_set_pairing(voice_ble_device_name());
            break;
        case APP_EVENT_PAIRING_CLOSED:
            s_pairing_open = false;
            render_display_state();
            break;
        case APP_EVENT_POWER_IRQ:
            board_power_events_rearm();
            /* fall through */
        case APP_EVENT_BATTERY_REFRESH:
            update_battery_status();
            break;
        case APP_EVENT_INACTIVITY_TIMEOUT:
            if (!s_audio_stream.capturing && !s_imu_stream.active && !s_ota_updating) {
                if (!s_display_dimmed) {
                    esp_err_t err = ui_status_set_brightness(DISPLAY_DIM_BRIGHTNESS);
                    if (err == ESP_OK) {
                        s_display_dimmed = true;
                        ui_status_set_idle_dimmed(true);
                        ESP_LOGI(TAG, "display dimmed after inactivity");
                    }
                }
            }
            break;
        case APP_EVENT_HANDSHAKE_TIMEOUT:
            s_handshake_window_open = false;
            update_ble_link_speed();
            break;
        case APP_EVENT_ENTER_DEEP_SLEEP:
            enter_deep_sleep(false);
            break;
        case APP_EVENT_OTA_BEGIN:
            s_ota_updating = true;
            close_all_streams(true, "ota");
            stop_fast_downlink_window();
            if (acquire_ota_pm_locks() != ESP_OK) {
                ESP_LOGW(TAG, "acquire OTA pm lock failed");
            }
            note_activity();
            ui_status_set_ota_progress(event.written, event.size);
            break;
        case APP_EVENT_OTA_PROGRESS:
            s_ota_updating = true;
            ui_status_set_ota_progress(event.written, event.size);
            break;
        case APP_EVENT_OTA_DONE:
            ui_status_set_ota_rebooting();
            break;
        case APP_EVENT_OTA_END:
            s_ota_updating = false;
            release_ota_pm_locks();
            s_display_scene = DISPLAY_SCENE_IDLE;
            s_display_text[0] = '\0';
            note_activity();
            render_display_state();
            break;
        }
    }
}

static esp_err_t init_gpio_button(gpio_num_t gpio_num, button_handle_t *button)
{
    const button_config_t button_config = {0};
    const button_gpio_config_t gpio_config = {
        .gpio_num = gpio_num,
        .active_level = 0,
        .enable_power_save = true,
    };
    return iot_button_new_gpio_device(&button_config, &gpio_config, button);
}

static esp_err_t init_buttons(void)
{
    s_app_event_queue = xQueueCreate(16, sizeof(app_event_t));
    if (!s_app_event_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_button_count = board_buttons(&s_buttons);
    if (s_button_count == 0 || s_button_count > MAX_BUTTONS) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_buttons_mask = 0;
    for (size_t i = 0; i < s_button_count; ++i) {
        esp_err_t err = init_gpio_button(s_buttons[i].gpio, &s_button_handles[i]);
        if (err != ESP_OK) {
            return err;
        }
        if (gpio_get_level(s_buttons[i].gpio) == 0) {
            s_buttons_mask |= (1u << i);
        }
        void *index = (void *)(uintptr_t)i;
        err = iot_button_register_cb(s_button_handles[i], BUTTON_PRESS_DOWN, NULL,
                                     button_down_cb, index);
        if (err != ESP_OK) {
            return err;
        }
        err = iot_button_register_cb(s_button_handles[i], BUTTON_PRESS_UP, NULL,
                                     button_up_cb, index);
        if (err != ESP_OK) {
            return err;
        }
    }

    BaseType_t ok = xTaskCreate(app_event_task, "app_event_task", 5120,
                                NULL, 6, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void inactivity_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_INACTIVITY_TIMEOUT);
}

static esp_err_t init_inactivity_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = inactivity_timer_cb,
        .name = "inactivity",
    };
    return esp_timer_create(&timer_args, &s_inactivity_timer);
}

static void handshake_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_HANDSHAKE_TIMEOUT);
}

static esp_err_t init_handshake_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = handshake_timer_cb,
        .name = "handshake",
    };
    return esp_timer_create(&timer_args, &s_handshake_timer);
}

static void deep_sleep_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_ENTER_DEEP_SLEEP);
}

static esp_err_t init_deep_sleep_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = deep_sleep_timer_cb,
        .name = "deep_sleep",
    };
    return esp_timer_create(&timer_args, &s_deep_sleep_timer);
}

static void battery_refresh_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_BATTERY_REFRESH);
}

static esp_err_t init_battery_refresh_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = battery_refresh_timer_cb,
        .name = "battery_refresh",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_battery_refresh_timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_periodic(s_battery_refresh_timer, BATTERY_REFRESH_FALLBACK_US);
}

static void IRAM_ATTR power_event_cb(void)
{
    if (!xPortInIsrContext()) {
        queue_app_event(APP_EVENT_POWER_IRQ);
        return;
    }
    BaseType_t high_task_woken = pdFALSE;
    queue_app_event_from_isr(APP_EVENT_POWER_IRQ, &high_task_woken);
    if (high_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void update_battery_status(void)
{
    int level = 0;
    bool charging = false;
    bool usb_powered = false;
    esp_err_t err = board_battery_level(&level);
    if (err == ESP_OK) {
        err = board_battery_charging(&charging);
    }
    if (err == ESP_OK) {
        err = board_usb_powered(&usb_powered);
    }
    if (err == ESP_OK) {
        if (level < 0) {
            level = 0;
        } else if (level > 100) {
            level = 100;
        }
        const bool external_power_changed = !s_battery_valid ||
                                            (charging != s_battery_charging) ||
                                            (usb_powered != s_usb_powered);
        s_battery_valid = true;
        s_battery_percent = level;
        s_battery_charging = charging;
        s_usb_powered = usb_powered;
        ui_status_set_battery(level, charging, usb_powered);

        if (voice_ble_control_ready() &&
            (!s_battery_reported || level != s_last_reported_percent ||
             charging != s_last_reported_charging || usb_powered != s_last_reported_usb)) {
            char data[96];
            snprintf(data, sizeof(data),
                     "{\"percent\":%d,\"charging\":%s,\"usb_powered\":%s}",
                     level, charging ? "true" : "false", usb_powered ? "true" : "false");
            if (voice_ble_send_event("battery.changed", now_timestamp_ms(), data) == ESP_OK) {
                s_battery_reported = true;
                s_last_reported_percent = level;
                s_last_reported_charging = charging;
                s_last_reported_usb = usb_powered;
            }
        }

        if (external_power_changed) {
            ESP_LOGI(TAG, "power source changed charging=%d usb=%d", charging, usb_powered);
            restart_deep_sleep_timer();
        }
    } else {
        ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(err));
    }

    if (!is_external_powered() && board_battery_critical()) {
        ESP_LOGW(TAG, "battery critical, sleeping until USB is connected");
        ui_status_set_error("Battery low");
        enter_deep_sleep(true);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot reset_reason=%d wakeup_causes=0x%" PRIx32,
             esp_reset_reason(), esp_sleep_get_wakeup_causes());

    ESP_ERROR_CHECK(init_power_management());
    ESP_ERROR_CHECK(board_init());
    s_imu = board_imu();
    ESP_ERROR_CHECK(ui_status_init());
    ESP_ERROR_CHECK(init_inactivity_timer());
    ESP_ERROR_CHECK(init_handshake_timer());
    ESP_ERROR_CHECK(init_deep_sleep_timer());
    note_activity();
    voice_ble_set_connection_callback(ble_connection_cb);
    voice_ble_set_pairing_callback(ble_pairing_cb);
    voice_ble_set_control_callback(ble_control_cb);
    voice_ble_set_ota_callback(ble_ota_cb);
    ESP_ERROR_CHECK(init_buttons());

    esp_err_t err = voice_ble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(err));
        ui_status_set_error("BLE init failed");
    } else {
        ui_status_set_device_name(voice_ble_device_name());
    }

    esp_err_t audio_err = audio_pipeline_init();
    if (audio_err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(audio_err));
        ui_status_set_error("Audio init failed");
    }

    if (err == ESP_OK && !s_pairing_open) {
        ui_status_set_advertising();
    }
    ESP_LOGI(TAG, "Voice Stick booted (Protocol v%d)", VOICE_BLE_PROTOCOL_VERSION);

    update_battery_status();
    ESP_ERROR_CHECK(init_battery_refresh_timer());
    ESP_ERROR_CHECK(board_power_events_init(power_event_cb));

#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
    const esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
#endif
}
