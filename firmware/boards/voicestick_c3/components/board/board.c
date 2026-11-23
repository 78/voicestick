#include "board.h"

#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "voicestick_c3.h"

static const char *TAG = "board";

#define BATTERY_ADC_SAMPLES 16
#define BATTERY_CRITICAL_MV 3500
/* Consecutive low readings (10 s apart) before shutting down, so a load
 * transient during a BLE burst does not trigger it. */
#define BATTERY_CRITICAL_READINGS 3

static const board_button_t s_buttons[] = {
    {VS_PIN_KEY_MAIN, "primary"},
    {VS_PIN_KEY_CANCEL, "secondary"},
    {VS_PIN_KEY_PREV, "prev"},
    {VS_PIN_KEY_NEXT, "next"},
};

/* Resting LiPo voltage to remaining capacity, light load. */
static const struct {
    int mv;
    int percent;
} s_battery_curve[] = {
    {3500, 0}, {3600, 8}, {3680, 20}, {3740, 35}, {3800, 50},
    {3870, 62}, {3950, 75}, {4050, 88}, {4150, 100},
};

static board_power_event_cb_t s_power_event_cb;
static esp_pm_lock_handle_t s_usb_pm_lock;
static bool s_usb_pm_locked;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static adc_channel_t s_adc_channel;
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

static esp_err_t init_battery_adc(void)
{
    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(VS_PIN_BAT_ADC, &unit, &s_adc_channel);
    if (err != ESP_OK) {
        return err;
    }

    const adc_oneshot_unit_init_cfg_t unit_config = {.unit_id = unit};
    err = adc_oneshot_new_unit(&unit_config, &s_adc);
    if (err != ESP_OK) {
        return err;
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, s_adc_channel, &channel_config);
    if (err != ESP_OK) {
        return err;
    }

    const adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = unit,
        .chan = s_adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_cali_create_scheme_curve_fitting(&cali_config, &s_adc_cali);
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

    err = init_battery_adc();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC init failed: %s", esp_err_to_name(err));
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

static esp_err_t arm_vbus_event(void)
{
    const bool usb_powered = gpio_get_level(VS_PIN_VBUS_SENSE) != 0;
    if (usb_powered != s_usb_pm_locked) {
        // An APB lock keeps the PLL running for USB enumeration and prevents light sleep.
        esp_err_t err = usb_powered ? esp_pm_lock_acquire(s_usb_pm_lock) : esp_pm_lock_release(s_usb_pm_lock);
        if (err != ESP_OK) {
            return err;
        }
        s_usb_pm_locked = usb_powered;
    }

    // Keep VBUS interrupts while awake and leave GPIO5 isolated in light sleep.
    esp_err_t err = gpio_set_intr_type(VS_PIN_VBUS_SENSE, usb_powered ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_intr_enable(VS_PIN_VBUS_SENSE);
}

esp_err_t board_power_events_init(board_power_event_cb_t callback)
{
    s_power_event_cb = callback;

    // Keep monitoring CHRG by polling because the TP4054 toggles it without a battery.
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
    return arm_vbus_event();
}

void board_power_events_rearm(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(arm_vbus_event());
}

static esp_err_t read_battery_mv(int *battery_mv)
{
    if (!s_adc || !s_adc_cali) {
        return ESP_ERR_INVALID_STATE;
    }

    int sum = 0;
    for (int i = 0; i < BATTERY_ADC_SAMPLES; ++i) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_adc, s_adc_channel, &raw);
        if (err != ESP_OK) {
            return err;
        }
        sum += raw;
    }

    int adc_mv = 0;
    esp_err_t err = adc_cali_raw_to_voltage(s_adc_cali, sum / BATTERY_ADC_SAMPLES, &adc_mv);
    if (err == ESP_OK) {
        *battery_mv = adc_mv * 2;
    }
    return err;
}

static int battery_percent(int battery_mv)
{
    const size_t points = sizeof(s_battery_curve) / sizeof(s_battery_curve[0]);
    if (battery_mv <= s_battery_curve[0].mv) {
        return 0;
    }
    for (size_t i = 1; i < points; ++i) {
        if (battery_mv < s_battery_curve[i].mv) {
            const int span_mv = s_battery_curve[i].mv - s_battery_curve[i - 1].mv;
            const int span_percent = s_battery_curve[i].percent - s_battery_curve[i - 1].percent;
            return s_battery_curve[i - 1].percent +
                   (battery_mv - s_battery_curve[i - 1].mv) * span_percent / span_mv;
        }
    }
    return 100;
}

esp_err_t board_battery_level(int *level_percent)
{
    int battery_mv = 0;
    esp_err_t err = read_battery_mv(&battery_mv);
    if (err != ESP_OK) {
        return err;
    }

    if (battery_mv < BATTERY_CRITICAL_MV) {
        ++s_low_battery_readings;
    } else {
        s_low_battery_readings = 0;
    }
    static int last_percent = -1;
    *level_percent = battery_percent(battery_mv);
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
    /* CHRG floats without VBUS; only trust it while USB is present. */
    *charging = gpio_get_level(VS_PIN_VBUS_SENSE) == 1 && gpio_get_level(VS_PIN_CHRG) == 0;
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
