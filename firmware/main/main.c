#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button_gpio.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "iot_button.h"

#include "audio_pipeline.h"
#include "stick_s3_board.h"
#include "ui_status.h"
#include "voice_ble.h"

static const char *TAG = "voice_stick";

#define BATTERY_REFRESH_FALLBACK_MS (10 * 1000)
#define DISPLAY_DIM_TIMEOUT_MS (30 * 1000)
#define DISPLAY_ACTIVE_BRIGHTNESS 128
#define DISPLAY_DIM_BRIGHTNESS 32
#define DISPLAY_DIM_TIMEOUT_US (DISPLAY_DIM_TIMEOUT_MS * 1000ULL)
#define BATTERY_REFRESH_FALLBACK_US (BATTERY_REFRESH_FALLBACK_MS * 1000ULL)
#define DEEP_SLEEP_TIMEOUT_MS (5 * 60 * 1000)
#define DEEP_SLEEP_TIMEOUT_US (DEEP_SLEEP_TIMEOUT_MS * 1000ULL)

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
static uint32_t s_session_id = 1;
static QueueHandle_t s_app_event_queue;
static button_handle_t s_front_button;
static button_handle_t s_side_button;

typedef enum {
    APP_EVENT_FRONT_DOWN,
    APP_EVENT_FRONT_UP,
    APP_EVENT_SIDE_DOWN,
    APP_EVENT_BLE_CONNECTED,
    APP_EVENT_BLE_DISCONNECTED,
    APP_EVENT_POWER_IRQ,
    APP_EVENT_BATTERY_REFRESH,
    APP_EVENT_ENTER_DEEP_SLEEP,
    APP_EVENT_OTA_BEGIN,
    APP_EVENT_OTA_PROGRESS,
    APP_EVENT_OTA_DONE,
    APP_EVENT_OTA_END,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    uint32_t written;
    uint32_t size;
} app_event_t;

static void update_battery_status(void);
static void queue_app_event(app_event_type_t type);
static void queue_app_event_with_ota(app_event_type_t type, uint32_t written, uint32_t size);

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

static void enter_deep_sleep(void)
{
    if (s_recording || s_ota_updating || voice_ble_ota_is_active()) {
        restart_deep_sleep_timer();
        return;
    }

    if (is_external_powered()) {
        ESP_LOGI(TAG, "skip deep sleep while charging or USB powered");
        restart_deep_sleep_timer();
        return;
    }

    bool charging = false;
    bool usb_powered = false;
    esp_err_t power_err = stick_s3_board_battery_charging(&charging);
    if (power_err == ESP_OK) {
        power_err = stick_s3_board_usb_powered(&usb_powered);
    }
    if (power_err == ESP_OK && (charging || usb_powered)) {
        s_battery_charging = charging;
        s_usb_powered = usb_powered;
        ESP_LOGI(TAG, "skip deep sleep after fresh power check charging=%d usb=%d",
                 charging, usb_powered);
        restart_deep_sleep_timer();
        return;
    }

    const gpio_num_t wake_gpio = STICK_S3_PIN_BUTTON_FRONT;
    if (!esp_sleep_is_valid_wakeup_gpio(wake_gpio)) {
        ESP_LOGE(TAG, "GPIO%d cannot wake from deep sleep", wake_gpio);
        restart_deep_sleep_timer();
        return;
    }

    ESP_LOGI(TAG, "entering deep sleep, wake on front button GPIO%d low", wake_gpio);
    release_recording_pm_locks();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_status_set_brightness(0));
    ui_status_prepare_deep_sleep();
    stick_s3_board_prepare_deep_sleep();

    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_pull_mode(wake_gpio, GPIO_PULLUP_ONLY));
    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << wake_gpio,
                                                    ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable deep sleep wake failed: %s", esp_err_to_name(err));
        restart_deep_sleep_timer();
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

static void start_recording(void)
{
    if (s_recording || s_ota_updating || voice_ble_ota_is_active() ||
        !voice_ble_is_connected()) {
        return;
    }

    const uint32_t session_id = s_session_id++;
    esp_err_t err = acquire_recording_pm_locks();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "acquire recording pm locks failed: %s", esp_err_to_name(err));
        ui_status_set_error("Power lock failed");
        return;
    }

    err = audio_pipeline_start(session_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio start failed: %s", esp_err_to_name(err));
        release_recording_pm_locks();
        ui_status_set_error("Audio start failed");
        return;
    }

    s_recording = true;
    restart_display_dim_timer();
    restart_deep_sleep_timer();
    ui_status_set_recording(session_id);
    voice_ble_send_press_start(session_id);
}

static void stop_recording(void)
{
    if (!s_recording) {
        return;
    }

    s_recording = false;
    audio_pipeline_stop();
    voice_ble_send_press_end(audio_pipeline_session_id());
    release_recording_pm_locks();
    ui_status_set_idle();
    restart_display_dim_timer();
    restart_deep_sleep_timer();
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

static void front_button_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_FRONT_DOWN);
}

static void front_button_up_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_FRONT_UP);
}

static void side_button_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_SIDE_DOWN);
}

static void ble_connection_cb(bool connected)
{
    queue_app_event(connected ? APP_EVENT_BLE_CONNECTED : APP_EVENT_BLE_DISCONNECTED);
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
        case APP_EVENT_FRONT_DOWN:
            note_activity();
            start_recording();
            break;
        case APP_EVENT_FRONT_UP:
            note_activity();
            stop_recording();
            break;
        case APP_EVENT_SIDE_DOWN:
            note_activity();
            if (!s_recording) {
                voice_ble_send_cancel();
            }
            break;
        case APP_EVENT_BLE_CONNECTED:
            ui_status_set_idle();
            break;
        case APP_EVENT_BLE_DISCONNECTED:
            s_recording = false;
            s_ota_updating = false;
            audio_pipeline_stop();
            release_recording_pm_locks();
            release_ota_pm_locks();
            ui_status_set_pairing(voice_ble_device_name());
            break;
        case APP_EVENT_POWER_IRQ:
        case APP_EVENT_BATTERY_REFRESH:
            update_battery_status();
            break;
        case APP_EVENT_ENTER_DEEP_SLEEP:
            enter_deep_sleep();
            break;
        case APP_EVENT_OTA_BEGIN:
            s_ota_updating = true;
            if (s_recording) {
                s_recording = false;
                audio_pipeline_stop();
                voice_ble_send_press_end(audio_pipeline_session_id());
                release_recording_pm_locks();
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
            ui_status_set_idle();
            restart_display_dim_timer();
            restart_deep_sleep_timer();
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
    };

    return iot_button_new_gpio_device(&button_config, &gpio_config, button);
}

static esp_err_t init_buttons(void)
{
    s_app_event_queue = xQueueCreate(12, sizeof(app_event_t));
    if (!s_app_event_queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = init_gpio_button(STICK_S3_PIN_BUTTON_FRONT, &s_front_button);
    if (err != ESP_OK) {
        return err;
    }
    err = init_gpio_button(STICK_S3_PIN_BUTTON_SIDE, &s_side_button);
    if (err != ESP_OK) {
        return err;
    }

    err = iot_button_register_cb(s_front_button, BUTTON_PRESS_DOWN, NULL,
                                 front_button_down_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = iot_button_register_cb(s_front_button, BUTTON_PRESS_UP, NULL,
                                 front_button_up_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = iot_button_register_cb(s_side_button, BUTTON_PRESS_DOWN, NULL,
                                 side_button_down_cb, NULL);
    if (err != ESP_OK) {
        return err;
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

static void IRAM_ATTR pmic_irq_isr(void *arg)
{
    (void)arg;

    BaseType_t high_task_woken = pdFALSE;
    queue_app_event_from_isr(APP_EVENT_POWER_IRQ, &high_task_woken);
    if (high_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t init_pmic_irq(void)
{
    gpio_config_t irq_config = {
        .pin_bit_mask = 1ULL << STICK_S3_PIN_PMIC_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&irq_config);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    return gpio_isr_handler_add(STICK_S3_PIN_PMIC_IRQ, pmic_irq_isr, NULL);
}

static void update_battery_status(void)
{
    uint8_t sys_status = 0;
    esp_err_t irq_err = stick_s3_board_clear_power_irqs(&sys_status);
    if (irq_err == ESP_OK && sys_status) {
        ESP_LOGI(TAG, "PMIC sys irq=0x%02x", sys_status);
    }

    int level = 0;
    bool charging = false;
    bool usb_powered = false;
    esp_err_t err = stick_s3_board_battery_level(&level);
    if (err == ESP_OK) {
        err = stick_s3_board_battery_charging(&charging);
    }
    if (err == ESP_OK) {
        err = stick_s3_board_usb_powered(&usb_powered);
    }
    if (err == ESP_OK) {
        const bool external_power_changed = (charging != s_battery_charging) ||
                                            (usb_powered != s_usb_powered);
        s_battery_charging = charging;
        s_usb_powered = usb_powered;
        ui_status_set_battery(level, charging, usb_powered);
        if (external_power_changed) {
            ESP_LOGI(TAG, "power source changed charging=%d usb=%d",
                     charging, usb_powered);
            restart_deep_sleep_timer();
        }
    } else {
        ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot reset_reason=%d wakeup_cause=%d",
             esp_reset_reason(), esp_sleep_get_wakeup_cause());

    ESP_ERROR_CHECK(init_power_management());
    ESP_ERROR_CHECK(stick_s3_board_init());
    ESP_ERROR_CHECK(ui_status_init());
    ESP_ERROR_CHECK(init_display_dim_timer());
    ESP_ERROR_CHECK(init_deep_sleep_timer());
    note_activity();
    voice_ble_set_connection_callback(ble_connection_cb);
    voice_ble_set_ota_callback(ble_ota_cb);

    esp_err_t err = voice_ble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(err));
        ui_status_set_error("BLE init failed");
    }

    esp_err_t audio_err = audio_pipeline_init();
    if (audio_err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(audio_err));
        ui_status_set_error("Audio init failed");
    }

    if (err == ESP_OK) {
        ui_status_set_pairing(voice_ble_device_name());
    }
    ESP_LOGI(TAG, "Voice Stick booted");

    ESP_ERROR_CHECK(init_buttons());
    update_battery_status();
    ESP_ERROR_CHECK(init_battery_refresh_timer());
    ESP_ERROR_CHECK(init_pmic_irq());
}
