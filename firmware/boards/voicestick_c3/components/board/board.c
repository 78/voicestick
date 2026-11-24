#include "board.h"

#include <stdlib.h>

#include "adc_battery_estimation.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "voicestick_c3.h"

static const char *TAG = "board";

#define BATTERY_CRITICAL_MV 3500
/* Require consecutive low readings so a brief load transient cannot trigger shutdown. */
#define BATTERY_CRITICAL_READINGS 3
/* The TP4054 can pulse CHRG when powered without a battery. */
#define CHARGE_DEBOUNCE_TICKS pdMS_TO_TICKS(2200)

static const board_button_t s_buttons[] = {
    {VS_PIN_KEY_MAIN, "primary"},
    {VS_PIN_KEY_CANCEL, "secondary"},
    {VS_PIN_KEY_PREV, "prev"},
    {VS_PIN_KEY_NEXT, "next"},
};

/* Resting LiPo voltage to remaining capacity, in descending voltage order. */
static const battery_point_t s_battery_curve[] = {
    {4.15f, 100}, {4.05f, 88}, {3.95f, 75}, {3.87f, 62}, {3.80f, 50},
    {3.74f, 35}, {3.68f, 20}, {3.60f, 8}, {BATTERY_CRITICAL_MV / 1000.0f, 0},
};

static board_power_event_cb_t s_power_event_cb;
static esp_pm_lock_handle_t s_usb_pm_lock;
static bool s_usb_pm_locked;
static TimerHandle_t s_charge_debounce_timer;
static portMUX_TYPE s_charge_lock = portMUX_INITIALIZER_UNLOCKED;
static TickType_t s_charge_edge_tick;
static bool s_charging;
static adc_battery_estimation_handle_t s_battery_estimator;
static int s_low_battery_readings;

static portMUX_TYPE s_periph_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_periph_users;

bool voicestick_periph_power_acquire(void)
{
    taskENTER_CRITICAL(&s_periph_lock);
    const bool powered_up = s_periph_users++ == 0;
    if (powered_up) {
        gpio_set_level(VS_PIN_PERIPH_EN, 0);
    }
    taskEXIT_CRITICAL(&s_periph_lock);
    return powered_up;
}

void voicestick_periph_power_release(void)
{
    taskENTER_CRITICAL(&s_periph_lock);
    if (s_periph_users > 0 && --s_periph_users == 0) {
        gpio_set_level(VS_PIN_PERIPH_EN, 1);
    }
    taskEXIT_CRITICAL(&s_periph_lock);
}

const char *board_hardware_name(void)
{
    return "voicestick_c3";
}

size_t board_buttons(const board_button_t **buttons)
{
    *buttons = s_buttons;
    return sizeof(s_buttons) / sizeof(s_buttons[0]);
}

static bool battery_is_charging(void *arg)
{
    (void)arg;
    /* CHRG floats without VBUS; only trust it while USB is present. */
    if (!gpio_get_level(VS_PIN_VBUS_SENSE)) {
        return false;
    }

    taskENTER_CRITICAL(&s_charge_lock);
    if (s_usb_pm_locked && xTaskGetTickCount() - s_charge_edge_tick >= CHARGE_DEBOUNCE_TICKS) {
        s_charging = gpio_get_level(VS_PIN_CHRG) == 0;
    }
    const bool charging = s_charging;
    taskEXIT_CRITICAL(&s_charge_lock);
    return charging;
}

static esp_err_t init_battery_estimator(void)
{
    adc_unit_t unit;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel(VS_PIN_BAT_ADC, &unit, &channel);
    if (err != ESP_OK) {
        return err;
    }

    adc_battery_estimation_t config = {
        .internal = {
            .adc_unit = unit,
            .adc_bitwidth = ADC_BITWIDTH_DEFAULT,
            .adc_atten = ADC_ATTEN_DB_12,
        },
        .adc_channel = channel,
        .upper_resistor = 1000000.0f,
        .lower_resistor = 1000000.0f,
        .battery_points = s_battery_curve,
        .battery_points_count = sizeof(s_battery_curve) / sizeof(s_battery_curve[0]),
        .charging_detect_cb = battery_is_charging,
    };
    s_battery_estimator = adc_battery_estimation_create(&config);
    return s_battery_estimator ? ESP_OK : ESP_FAIL;
}

esp_err_t board_init(void)
{
    /* Deep-sleep wake pins come back latched. */
    gpio_hold_dis(VS_PIN_KEY_MAIN);
    gpio_hold_dis(VS_PIN_VBUS_SENSE);

    const gpio_config_t rail_config = {
        .pin_bit_mask = BIT64(VS_PIN_PERIPH_EN),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_set_level(VS_PIN_PERIPH_EN, 1);
    if (err == ESP_OK) {
        err = gpio_config(&rail_config);
    }
    if (err != ESP_OK) {
        return err;
    }

    const gpio_config_t vbus_config = {
        .pin_bit_mask = BIT64(VS_PIN_VBUS_SENSE),
        .mode = GPIO_MODE_INPUT,
    };
    err = gpio_config(&vbus_config);
    if (err != ESP_OK) {
        return err;
    }

    /* Once charging is done CHRG still sinks ~20 uA through the charge LED;
     * the internal pull-up keeps the pin above VIH. */
    const gpio_config_t chrg_config = {
        .pin_bit_mask = BIT64(VS_PIN_CHRG),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    err = gpio_config(&chrg_config);
    if (err != ESP_OK) {
        return err;
    }

    /* PM_SLP_DISABLE_GPIO isolates every pad in light sleep; the rail switch
     * and the CHRG pull-up must keep their awake configuration. */
    gpio_sleep_sel_dis(VS_PIN_PERIPH_EN);
    gpio_sleep_sel_dis(VS_PIN_CHRG);

    err = init_battery_estimator();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery estimator init failed: %s", esp_err_to_name(err));
    }
#ifdef CONFIG_VOICE_STICK_PDM_CAPTURE
    voicestick_pdm_capture_start();
#endif
    return err;
}

static void IRAM_ATTR vbus_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(VS_PIN_VBUS_SENSE);
    if (s_power_event_cb) {
        s_power_event_cb();
    }
}

static void IRAM_ATTR charge_isr(void *arg)
{
    (void)arg;
    taskENTER_CRITICAL_ISR(&s_charge_lock);
    s_charge_edge_tick = xTaskGetTickCountFromISR();
    taskEXIT_CRITICAL_ISR(&s_charge_lock);

    BaseType_t high_task_woken = pdFALSE;
    xTimerResetFromISR(s_charge_debounce_timer, &high_task_woken);
    if (high_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void charge_debounce_cb(TimerHandle_t timer)
{
    (void)timer;
    if (gpio_get_level(VS_PIN_VBUS_SENSE) && s_power_event_cb) {
        s_power_event_cb();
    }
}

static esp_err_t arm_vbus_event(void)
{
    const bool usb_powered = gpio_get_level(VS_PIN_VBUS_SENSE) != 0;
    if (usb_powered != s_usb_pm_locked) {
        // An APB lock keeps the PLL running for USB enumeration and prevents light sleep.
        esp_err_t err = usb_powered ? esp_pm_lock_acquire(s_usb_pm_lock) : esp_pm_lock_release(s_usb_pm_lock);
        if (err != ESP_OK) {
            return err;
        }
        taskENTER_CRITICAL(&s_charge_lock);
        s_usb_pm_locked = usb_powered;
        s_charge_edge_tick = xTaskGetTickCount();
        s_charging = false;
        taskEXIT_CRITICAL(&s_charge_lock);

        if (usb_powered) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_intr_enable(VS_PIN_CHRG));
            (void)xTimerReset(s_charge_debounce_timer, 0);
        } else {
            ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_intr_disable(VS_PIN_CHRG));
            (void)xTimerStop(s_charge_debounce_timer, 0);
        }
    }

    // Set the level for both the interrupt and the light-sleep wake source.
    esp_err_t err = gpio_wakeup_enable(VS_PIN_VBUS_SENSE, usb_powered ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_intr_enable(VS_PIN_VBUS_SENSE);
}

esp_err_t board_power_events_init(board_power_event_cb_t callback)
{
    s_power_event_cb = callback;

    s_charge_debounce_timer = xTimerCreate("charge_debounce", CHARGE_DEBOUNCE_TICKS,
                                          pdFALSE, NULL, charge_debounce_cb);
    if (!s_charge_debounce_timer) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "usb_vbus", &s_usb_pm_lock);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = gpio_isr_handler_add(VS_PIN_VBUS_SENSE, vbus_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }
    // Adding an ISR enables the pin. Set the trigger only after masking it.
    err = gpio_isr_handler_add(VS_PIN_CHRG, charge_isr, NULL);
    if (err == ESP_OK) {
        err = gpio_intr_disable(VS_PIN_CHRG);
    }
    if (err == ESP_OK) {
        err = gpio_set_intr_type(VS_PIN_CHRG, GPIO_INTR_ANYEDGE);
    }
    if (err != ESP_OK) {
        return err;
    }
    return arm_vbus_event();
}

void board_power_events_rearm(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(arm_vbus_event());
}

esp_err_t board_battery_level(int *level_percent)
{
    if (!level_percent) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Check shutdown against the latest voltage sample, before capacity smoothing. */
    float voltage;
    esp_err_t err = adc_battery_estimation_get_voltage(s_battery_estimator, &voltage);
    if (err != ESP_OK) {
        return err;
    }

    const int battery_mv = (int)(voltage * 1000.0f + 0.5f);
    if (battery_mv >= BATTERY_CRITICAL_MV) {
        s_low_battery_readings = 0;
    } else if (s_low_battery_readings < BATTERY_CRITICAL_READINGS) {
        ++s_low_battery_readings;
    }

    float capacity;
    err = adc_battery_estimation_get_capacity(s_battery_estimator, &capacity);
    if (err != ESP_OK) {
        return err;
    }

    static int last_percent = -1;
    *level_percent = (int)(capacity + 0.5f);
    if (last_percent < 0 || abs(*level_percent - last_percent) >= 5) {
        ESP_LOGI(TAG, "battery %d mV (%d%%)", battery_mv, *level_percent);
        last_percent = *level_percent;
    }
    return ESP_OK;
}

esp_err_t board_usb_powered(bool *usb_powered)
{
    *usb_powered = gpio_get_level(VS_PIN_VBUS_SENSE) == 1;
    return ESP_OK;
}

esp_err_t board_battery_charging(bool *charging)
{
    *charging = battery_is_charging(NULL);
    return ESP_OK;
}

bool board_battery_critical(void)
{
    return s_low_battery_readings >= BATTERY_CRITICAL_READINGS;
}

static void release_pin(gpio_num_t pin)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
}

esp_err_t board_prepare_deep_sleep(bool usb_wake_only)
{
    int wait_ms = 0;
    while (!usb_wake_only && gpio_get_level(VS_PIN_KEY_MAIN) == 0 && wait_ms < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (!usb_wake_only && gpio_get_level(VS_PIN_KEY_MAIN) == 0) {
        ESP_LOGW(TAG, "main key still low after %d ms, abort deep sleep", wait_ms);
        return ESP_ERR_INVALID_STATE;
    }

    /* Nothing may drive into the unpowered LED and mic. R13 holds the rail
     * off once PERIPH_EN is released. */
    release_pin(VS_PIN_PDM_CLK);
    release_pin(VS_PIN_PDM_DAT);
    release_pin(VS_PIN_LED_DIN);
    release_pin(VS_PIN_PERIPH_EN);
    gpio_pullup_dis(VS_PIN_CHRG);

    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    /* Both wake pins have external resistors, so the IDF must not add its
     * own (CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS=n): an internal
     * pull-down on VBUS_SENSE would pull USB-present below VIH. */
    esp_err_t err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
        BIT64(VS_PIN_VBUS_SENSE), ESP_GPIO_WAKEUP_GPIO_HIGH);
    if (err == ESP_OK && !usb_wake_only) {
        err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
            BIT64(VS_PIN_KEY_MAIN), ESP_GPIO_WAKEUP_GPIO_LOW);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable deep sleep wake failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "deep sleep armed, wake on %s", usb_wake_only ? "USB" : "main key or USB");
    return ESP_OK;
}
