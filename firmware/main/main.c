#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button_gpio.h"
#include "cJSON.h"
#include "driver/gpio.h"
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
#define DISPLAY_DIM_TIMEOUT_MS (30 * 1000)
#define DISPLAY_ACTIVE_BRIGHTNESS 128
#define DISPLAY_DIM_BRIGHTNESS 32
#define DISPLAY_DIM_TIMEOUT_US (DISPLAY_DIM_TIMEOUT_MS * 1000ULL)
#define DEEP_SLEEP_TIMEOUT_US (CONFIG_VOICE_STICK_DEEP_SLEEP_TIMEOUT_S * 1000000ULL)
#define PAIRING_HOLD_MS 3000
#define MAX_BUTTONS 4

static bool s_recording;
static bool s_ota_updating;
static bool s_display_dimmed;
static bool s_recording_pm_locked;
static bool s_ota_pm_locked;
static bool s_battery_charging;
static bool s_usb_powered;
static esp_pm_lock_handle_t s_cpu_freq_lock;
static esp_timer_handle_t s_display_dim_timer;
static esp_timer_handle_t s_deep_sleep_timer;
static esp_timer_handle_t s_battery_refresh_timer;
static esp_timer_handle_t s_host_response_timer;
static uint32_t s_session_id = 1;
static QueueHandle_t s_app_event_queue;
static const board_button_t *s_buttons;
static size_t s_button_count;
static button_handle_t s_button_handles[MAX_BUTTONS];
static int64_t s_button_down_us[MAX_BUTTONS];
static int64_t s_primary_down_us;
static uint32_t s_primary_session_id;

typedef enum {
    APP_UI_STATE_READY,
    APP_UI_STATE_RECORDING,
    APP_UI_STATE_THINKING,
    APP_UI_STATE_PENDING_CONFIRMATION,
    APP_UI_STATE_ERROR,
} app_ui_state_t;

static app_ui_state_t s_app_ui_state = APP_UI_STATE_READY;

typedef enum {
    INTERACTION_MODE_HOLD_TO_TALK,
    INTERACTION_MODE_CLICK_TO_TALK,
} interaction_mode_t;

static interaction_mode_t s_interaction_mode = INTERACTION_MODE_HOLD_TO_TALK;

static const char *app_ui_state_name(app_ui_state_t state)
{
    switch (state) {
    case APP_UI_STATE_READY:
        return "ready";
    case APP_UI_STATE_RECORDING:
        return "recording";
    case APP_UI_STATE_THINKING:
        return "thinking";
    case APP_UI_STATE_PENDING_CONFIRMATION:
        return "pending_confirmation";
    case APP_UI_STATE_ERROR:
        return "error";
    }
    return "unknown";
}

typedef enum {
    APP_EVENT_PRIMARY_DOWN,
    APP_EVENT_PRIMARY_UP,
    APP_EVENT_BUTTON_DOWN,
    APP_EVENT_BUTTON_UP,
    APP_EVENT_PAIRING_HOLD,
    APP_EVENT_UI_STATE,
    APP_EVENT_BLE_CONNECTED,
    APP_EVENT_BLE_DISCONNECTED,
    APP_EVENT_PAIRING_OPENED,
    APP_EVENT_PAIRING_CLOSED,
    APP_EVENT_POWER_IRQ,
    APP_EVENT_BATTERY_REFRESH,
    APP_EVENT_ENTER_DEEP_SLEEP,
    APP_EVENT_OTA_BEGIN,
    APP_EVENT_OTA_PROGRESS,
    APP_EVENT_OTA_DONE,
    APP_EVENT_OTA_END,
    APP_EVENT_HOST_RESPONSE_TIMEOUT,
} app_event_type_t;

static bool s_pairing_open;
static bool s_pairing_hold_handled;

typedef struct {
    app_event_type_t type;
    uint32_t button;
    uint32_t written;
    uint32_t size;
    char state[32];
    char text[96];
} app_event_t;

static void update_battery_status(void);
static uint32_t stop_recording(void);
static void queue_app_event(app_event_type_t type);
static void queue_app_event_with_ota(app_event_type_t type, uint32_t written, uint32_t size);
static void queue_ui_state_event(const char *state, const char *text);
static void apply_interaction_mode(interaction_mode_t mode);

static bool is_external_powered(void)
{
    return s_battery_charging || s_usb_powered;
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
    return ESP_OK;
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

static void restart_display_dim_timer(void)
{
    if (!s_display_dim_timer) {
        return;
    }

    (void)esp_timer_stop(s_display_dim_timer);
    if (!s_recording && !s_ota_updating) {
        esp_err_t err = esp_timer_start_once(s_display_dim_timer, DISPLAY_DIM_TIMEOUT_US);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "start dim timer failed: %s", esp_err_to_name(err));
        }
    }
}

static void restart_deep_sleep_timer(void)
{
    if (!s_deep_sleep_timer) {
        return;
    }

    (void)esp_timer_stop(s_deep_sleep_timer);
    if (!s_recording && !s_ota_updating && !is_external_powered()) {
        esp_err_t err = esp_timer_start_once(s_deep_sleep_timer, DEEP_SLEEP_TIMEOUT_US);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "start deep sleep timer failed: %s", esp_err_to_name(err));
        }
    } else if (is_external_powered()) {
        ESP_LOGD(TAG, "deep sleep timer paused while external power is present");
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
    restart_display_dim_timer();
    restart_deep_sleep_timer();
}

static void stop_host_response_timer(void)
{
    if (s_host_response_timer) {
        (void)esp_timer_stop(s_host_response_timer);
    }
}

static uint32_t elapsed_button_ms(int64_t down_us)
{
    if (down_us <= 0) {
        return 0;
    }
    int64_t elapsed_us = esp_timer_get_time() - down_us;
    if (elapsed_us < 0) {
        elapsed_us = 0;
    }
    return (uint32_t)(elapsed_us / 1000);
}

static void enter_deep_sleep(bool low_battery)
{
    if (s_ota_updating || voice_ble_ota_is_active() || (s_pairing_open && !low_battery)) {
        restart_deep_sleep_timer();
        return;
    }
    if (s_recording) {
        if (!low_battery) {
            restart_deep_sleep_timer();
            return;
        }
        const uint32_t session_id = stop_recording();
        (void)voice_ble_send_button_up(s_buttons[0].name, elapsed_button_ms(s_primary_down_us),
                                       session_id);
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

static bool app_ui_allows_recording_start(void)
{
    return s_app_ui_state != APP_UI_STATE_PENDING_CONFIRMATION;
}

static uint32_t start_recording(void)
{
    const bool ble_ready = voice_ble_is_ready();
    const bool ota_active = voice_ble_ota_is_active();
    const bool ui_allows_start = app_ui_allows_recording_start();
    if (s_recording || s_ota_updating || ota_active || !ble_ready || !ui_allows_start) {
        ESP_LOGW(TAG,
                 "start recording denied: recording=%d ota=%d ble_ota=%d ble_ready=%d ui_state=%d",
                 s_recording, s_ota_updating, ota_active, ble_ready, s_app_ui_state);
        return 0;
    }

    const uint32_t session_id = s_session_id++;
    esp_err_t err = acquire_recording_pm_locks();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "acquire recording pm locks failed: %s", esp_err_to_name(err));
        ui_status_set_error("Power lock failed");
        return 0;
    }

    err = audio_pipeline_start(session_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio start failed: %s", esp_err_to_name(err));
        release_recording_pm_locks();
        ui_status_set_error("Audio start failed");
        return 0;
    }

    s_recording = true;
    s_app_ui_state = APP_UI_STATE_RECORDING;
    restart_display_dim_timer();
    restart_deep_sleep_timer();
    ui_status_set_recording(session_id);
    return session_id;
}

static uint32_t stop_recording(void)
{
    if (!s_recording) {
        return 0;
    }

    const uint32_t session_id = audio_pipeline_session_id();
    s_recording = false;
    audio_pipeline_stop();
    release_recording_pm_locks();
    restart_display_dim_timer();
    restart_deep_sleep_timer();
    return session_id;
}

static void queue_app_event(app_event_type_t type)
{
    queue_app_event_with_ota(type, 0, 0);
}

static void queue_app_event_with_ota(app_event_type_t type, uint32_t written, uint32_t size)
{
    if (s_app_event_queue) {
        app_event_t event = {
            .type = type,
            .written = written,
            .size = size,
        };
        (void)xQueueSend(s_app_event_queue, &event, 0);
    }
}

static void queue_app_event_from_isr(app_event_type_t type, BaseType_t *high_task_woken)
{
    if (s_app_event_queue) {
        app_event_t event = {
            .type = type,
            .written = 0,
            .size = 0,
        };
        (void)xQueueSendFromISR(s_app_event_queue, &event, high_task_woken);
    }
}

static void queue_ui_state_event(const char *state, const char *text)
{
    if (!s_app_event_queue) {
        ESP_LOGW(TAG, "drop ui_state state=%s text_len=%u: app queue unavailable",
                 state ? state : "nil",
                 (unsigned)(text ? strlen(text) : 0));
        return;
    }

    app_event_t event = {
        .type = APP_EVENT_UI_STATE,
    };
    if (state) {
        strlcpy(event.state, state, sizeof(event.state));
    }
    if (text) {
        strlcpy(event.text, text, sizeof(event.text));
    }
    ESP_LOGI(TAG, "queue ui_state state=%s text_len=%u current=%s recording=%d",
             event.state[0] ? event.state : "nil",
             (unsigned)strlen(event.text),
             app_ui_state_name(s_app_ui_state),
             s_recording);
    if (xQueueSend(s_app_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop ui_state state=%s: app queue full",
                 event.state[0] ? event.state : "nil");
    }
}

static void queue_button_event(app_event_type_t type, uint32_t button)
{
    if (s_app_event_queue) {
        app_event_t event = {
            .type = type,
            .button = button,
        };
        (void)xQueueSend(s_app_event_queue, &event, 0);
    }
}

static void button_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    const uint32_t button = (uint32_t)(uintptr_t)usr_data;
    queue_button_event(button == 0 ? APP_EVENT_PRIMARY_DOWN : APP_EVENT_BUTTON_DOWN, button);
}

static void button_up_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    const uint32_t button = (uint32_t)(uintptr_t)usr_data;
    queue_button_event(button == 0 ? APP_EVENT_PRIMARY_UP : APP_EVENT_BUTTON_UP, button);
}

static void pairing_hold_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_PAIRING_HOLD);
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
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "ignore invalid control json");
        return;
    }

    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (cJSON_IsString(event) && strcmp(event->valuestring, "ui_state") == 0 &&
        cJSON_IsString(state)) {
        queue_ui_state_event(state->valuestring, cJSON_IsString(text) ? text->valuestring : "");
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "interaction_mode") == 0 &&
               cJSON_IsString(mode)) {
        if (strcmp(mode->valuestring, "click_to_talk") == 0) {
            apply_interaction_mode(INTERACTION_MODE_CLICK_TO_TALK);
        } else if (strcmp(mode->valuestring, "hold_to_talk") == 0) {
            apply_interaction_mode(INTERACTION_MODE_HOLD_TO_TALK);
        } else {
            ESP_LOGW(TAG, "unknown interaction_mode %s", mode->valuestring);
        }
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "get_device_info") == 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(voice_ble_send_device_info());
    }
    cJSON_Delete(root);
}

static void apply_app_ui_state(const char *state, const char *text)
{
    ESP_LOGI(TAG, "apply ui_state state=%s text_len=%u current=%s recording=%d",
             state ? state : "nil",
             (unsigned)(text ? strlen(text) : 0),
             app_ui_state_name(s_app_ui_state),
             s_recording);
    stop_host_response_timer();
    if (strcmp(state, "ready") == 0) {
        if (s_recording) {
            ESP_LOGI(TAG, "ignore ready ui_state while recording");
            return;
        }
        s_app_ui_state = APP_UI_STATE_READY;
        ui_status_set_idle();
        note_activity();
        voice_ble_request_slow_interval();
    } else if (strcmp(state, "recording") == 0) {
        s_app_ui_state = APP_UI_STATE_RECORDING;
        if (!s_recording) {
            ui_status_set_recording(0);
        }
    } else if (strcmp(state, "thinking") == 0) {
        s_app_ui_state = APP_UI_STATE_THINKING;
        ui_status_set_partial_text("");
    } else if (strcmp(state, "pending_confirmation") == 0) {
        s_app_ui_state = APP_UI_STATE_PENDING_CONFIRMATION;
        ui_status_set_partial_text("Confirm or cancel");
    } else if (strcmp(state, "error") == 0) {
        s_app_ui_state = APP_UI_STATE_ERROR;
        ui_status_set_error(text && text[0] ? text : "Unknown error");
    } else {
        ESP_LOGW(TAG, "unknown ui_state %s", state);
    }
    ESP_LOGI(TAG, "applied ui_state current=%s recording=%d",
             app_ui_state_name(s_app_ui_state),
             s_recording);
}

static void apply_interaction_mode(interaction_mode_t mode)
{
    s_interaction_mode = mode;
    ui_status_set_idle_hint(mode == INTERACTION_MODE_CLICK_TO_TALK ? "Click to Talk" : "Hold to Talk");
    if (s_app_ui_state == APP_UI_STATE_READY && !s_recording) {
        ui_status_set_idle();
    }
    ESP_LOGI(TAG, "interaction mode %s",
             mode == INTERACTION_MODE_CLICK_TO_TALK ? "click_to_talk" : "hold_to_talk");
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
        if (xQueueReceive(s_app_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {
        case APP_EVENT_PRIMARY_DOWN:
            ESP_LOGI(TAG, "button %s down", s_buttons[0].name);
            note_activity();
            if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK && s_recording) {
                const uint32_t primary_duration_ms = elapsed_button_ms(s_primary_down_us);
                s_primary_session_id = stop_recording();
                esp_err_t primary_up_err = voice_ble_send_button_click("primary", primary_duration_ms,
                                                                       s_primary_session_id);
                if (s_primary_session_id != 0 && primary_up_err != ESP_OK) {
                    apply_app_ui_state("ready", "");
                }
                s_primary_down_us = 0;
                s_primary_session_id = 0;
            } else {
                s_primary_down_us = esp_timer_get_time();
                if (s_app_ui_state == APP_UI_STATE_PENDING_CONFIRMATION) {
                    ESP_LOGI(TAG, "button primary down as pending confirmation control");
                    s_primary_session_id = 0;
                    (void)voice_ble_send_button_click("primary", 0, 0);
                    break;
                }
                s_primary_session_id = start_recording();
                if (s_primary_session_id == 0) {
                    s_primary_down_us = 0;
                    break;
                }
                esp_err_t primary_down_err = s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK
                    ? voice_ble_send_button_click("primary", 0, s_primary_session_id)
                    : voice_ble_send_button_down("primary", s_primary_session_id);
                if (s_primary_session_id != 0 && primary_down_err != ESP_OK) {
                    (void)stop_recording();
                    s_primary_session_id = 0;
                    apply_app_ui_state("ready", "");
                }
            }
            break;
        case APP_EVENT_PRIMARY_UP:
            ESP_LOGI(TAG, "button %s up", s_buttons[0].name);
            note_activity();
            if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK) {
                break;
            }
            if (!s_recording && s_primary_session_id == 0 && s_primary_down_us == 0) {
                break;
            }
            const uint32_t primary_duration_ms = elapsed_button_ms(s_primary_down_us);
            if (s_recording) {
                s_primary_session_id = stop_recording();
            }
            esp_err_t primary_up_err = voice_ble_send_button_up("primary", primary_duration_ms,
                                                                s_primary_session_id);
            if (s_primary_session_id != 0 && primary_up_err != ESP_OK) {
                apply_app_ui_state("ready", "");
            }
            s_primary_down_us = 0;
            s_primary_session_id = 0;
            break;
        case APP_EVENT_BUTTON_DOWN:
            ESP_LOGI(TAG, "button %s down", s_buttons[event.button].name);
            note_activity();
            s_button_down_us[event.button] = esp_timer_get_time();
            break;
        case APP_EVENT_BUTTON_UP:
            ESP_LOGI(TAG, "button %s up", s_buttons[event.button].name);
            note_activity();
            if (s_pairing_hold_handled && strcmp(s_buttons[event.button].name, "secondary") == 0) {
                s_pairing_hold_handled = false;
            } else {
                voice_ble_send_button_click(s_buttons[event.button].name,
                                            elapsed_button_ms(s_button_down_us[event.button]), 0);
            }
            s_button_down_us[event.button] = 0;
            break;
        case APP_EVENT_PAIRING_HOLD:
            if (!s_recording && !s_ota_updating) {
                s_pairing_hold_handled = true;
                voice_ble_open_pairing();
            }
            break;
        case APP_EVENT_UI_STATE:
            apply_app_ui_state(event.state, event.text);
            break;
        case APP_EVENT_BLE_CONNECTED:
            s_app_ui_state = APP_UI_STATE_READY;
            ui_status_set_idle();
            note_activity();
            break;
        case APP_EVENT_BLE_DISCONNECTED: {
            const bool was_recording = s_recording;
            s_recording = false;
            s_ota_updating = false;
            s_app_ui_state = APP_UI_STATE_READY;
            stop_host_response_timer();
            audio_pipeline_stop();
            release_recording_pm_locks();
            release_ota_pm_locks();
            if (was_recording) {
                restart_deep_sleep_timer();
            }
            if (!s_pairing_open) {
                ui_status_set_advertising();
            }
            break;
        }
        case APP_EVENT_PAIRING_OPENED:
            s_pairing_open = true;
            note_activity();
            ui_status_set_pairing(voice_ble_device_name());
            break;
        case APP_EVENT_PAIRING_CLOSED:
            s_pairing_open = false;
            if (!voice_ble_is_connected()) {
                ui_status_set_advertising();
            }
            break;
        case APP_EVENT_POWER_IRQ:
            board_power_events_rearm();
            /* fall through */
        case APP_EVENT_BATTERY_REFRESH:
            update_battery_status();
            break;
        case APP_EVENT_ENTER_DEEP_SLEEP:
            enter_deep_sleep(false);
            break;
        case APP_EVENT_OTA_BEGIN:
            s_ota_updating = true;
            if (s_recording) {
                const uint32_t session_id = stop_recording();
                voice_ble_send_button_up("primary", elapsed_button_ms(s_primary_down_us),
                                         session_id);
            }
            esp_err_t ota_pm_err = acquire_ota_pm_locks();
            if (ota_pm_err != ESP_OK) {
                ESP_LOGW(TAG, "acquire OTA pm lock failed: %s", esp_err_to_name(ota_pm_err));
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
            s_app_ui_state = APP_UI_STATE_READY;
            stop_host_response_timer();
            ui_status_set_idle();
            note_activity();
            break;
        case APP_EVENT_HOST_RESPONSE_TIMEOUT:
            if (!s_recording && (s_app_ui_state == APP_UI_STATE_RECORDING ||
                                 s_app_ui_state == APP_UI_STATE_THINKING)) {
                ESP_LOGW(TAG, "host response timeout, returning to ready");
                apply_app_ui_state("ready", "");
            }
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
        .enable_power_save = true
    };

    return iot_button_new_gpio_device(&button_config, &gpio_config, button);
}

static esp_err_t init_buttons(void)
{
    s_app_event_queue = xQueueCreate(12, sizeof(app_event_t));
    if (!s_app_event_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_button_count = board_buttons(&s_buttons);
    if (s_button_count == 0 || s_button_count > MAX_BUTTONS) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0; i < s_button_count; ++i) {
        esp_err_t err = init_gpio_button(s_buttons[i].gpio, &s_button_handles[i]);
        if (err != ESP_OK) {
            return err;
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
        if (strcmp(s_buttons[i].name, "secondary") == 0) {
            button_event_args_t args = {.long_press.press_time = PAIRING_HOLD_MS};
            err = iot_button_register_cb(s_button_handles[i], BUTTON_LONG_PRESS_START, &args,
                                         pairing_hold_cb, NULL);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    BaseType_t ok = xTaskCreate(app_event_task, "app_event_task", 4096,
                                NULL, 6, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void display_dim_timer_cb(void *arg)
{
    (void)arg;

    if (!s_display_dimmed && !s_recording && !s_ota_updating) {
        esp_err_t err = ui_status_set_brightness(DISPLAY_DIM_BRIGHTNESS);
        if (err == ESP_OK) {
            s_display_dimmed = true;
            ui_status_set_idle_dimmed(true);
            ESP_LOGI(TAG, "display dimmed after inactivity");
        } else {
            ESP_LOGW(TAG, "dim display failed: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t init_display_dim_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = display_dim_timer_cb,
        .name = "display_dim",
    };
    return esp_timer_create(&timer_args, &s_display_dim_timer);
}

static void deep_sleep_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_ENTER_DEEP_SLEEP);
}

static void host_response_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_HOST_RESPONSE_TIMEOUT);
}

static esp_err_t init_deep_sleep_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = deep_sleep_timer_cb,
        .name = "deep_sleep",
    };
    return esp_timer_create(&timer_args, &s_deep_sleep_timer);
}

static esp_err_t init_host_response_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = host_response_timer_cb,
        .name = "host_response",
    };
    return esp_timer_create(&timer_args, &s_host_response_timer);
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
        const bool external_power_changed = (charging != s_battery_charging) ||
                                            (usb_powered != s_usb_powered);
        s_battery_charging = charging;
        s_usb_powered = usb_powered;
        ui_status_set_battery(level, charging, usb_powered);
#ifdef CONFIG_VOICE_STICK_BLE_BATTERY_STATUS
        voice_ble_update_battery_status(level, charging, usb_powered);
#endif
        if (external_power_changed) {
            ESP_LOGI(TAG, "power source changed charging=%d usb=%d",
                     charging, usb_powered);
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
    ESP_ERROR_CHECK(ui_status_init());
    ESP_ERROR_CHECK(init_display_dim_timer());
    ESP_ERROR_CHECK(init_deep_sleep_timer());
    ESP_ERROR_CHECK(init_host_response_timer());
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

    if (err == ESP_OK) {
        ui_status_set_advertising();
    }
    ESP_LOGI(TAG, "Voice Stick booted");

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
