#include "board.h"

#include "adc_battery_estimation.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_sleep.h"

#include "es8311_input_board.h"
#include "magiclick_2p5.h"

static const char *TAG = "magiclick_2p5";

/* Pin assignments, PCB revision strap, and battery curve from the upstream
 * xiaozhi-esp32 main/boards/magiclick/2p5 port. */
#define PIN_KEY_MAIN GPIO_NUM_21
#define PIN_KEY_LEFT GPIO_NUM_0
#define PIN_KEY_RIGHT GPIO_NUM_47
#define PIN_CHARGING GPIO_NUM_48

/* ADC1 channels for GPIO3 and GPIO7, respectively. */
#define ADC_PCB_REVISION ADC_CHANNEL_2
#define ADC_BATTERY ADC_CHANNEL_6
/* MagiClick 2.5A schematic: R217 and R216 form the BAT_ADC divider. */
#define BATTERY_DIVIDER_UPPER_OHMS 100000.0f
#define BATTERY_DIVIDER_LOWER_OHMS 100000.0f

static const int s_battery_raw_at_percent[] = {1985, 2079, 2141, 2296, 2420, 2606};
static battery_point_t s_battery_curve[sizeof(s_battery_raw_at_percent) / sizeof(s_battery_raw_at_percent[0])];
static adc_battery_estimation_handle_t s_battery_estimator;

static const board_button_t s_buttons[] = {
    {PIN_KEY_MAIN, "primary"},
    {PIN_KEY_LEFT, "secondary"},
    {PIN_KEY_RIGHT, "next"},
};

static i2c_master_bus_handle_t s_codec_bus;
static adc_oneshot_unit_handle_t s_adc;
static bool s_gc9107;
static board_power_event_cb_t s_power_event_cb;
static const board_confirmation_motion_t *s_confirmation_motion;

static esp_err_t read_adc_average(adc_channel_t channel, int *average)
{
    int total = 0;
    for (int i = 0; i < 10; i++) {
        int raw;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, channel, &raw), TAG, "read ADC%d", channel);
        total += raw;
    }
    *average = total / 10;
    return ESP_OK;
}

static bool battery_is_charging(void *arg)
{
    (void)arg;
    return gpio_get_level(PIN_CHARGING) == 0;
}

static esp_err_t init_battery_estimator(const adc_oneshot_chan_cfg_t *channel)
{
    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_BATTERY,
        .atten = channel->atten,
        .bitwidth = channel->bitwidth,
    };
    adc_cali_handle_t calibration;
    ESP_RETURN_ON_ERROR(adc_cali_create_scheme_curve_fitting(&calibration_config, &calibration),
                        TAG, "calibrate battery ADC");

    const size_t point_count = sizeof(s_battery_curve) / sizeof(s_battery_curve[0]);
    const float battery_volts_per_adc_mv =
        (BATTERY_DIVIDER_UPPER_OHMS + BATTERY_DIVIDER_LOWER_OHMS) / BATTERY_DIVIDER_LOWER_OHMS / 1000.0f;
    /* Keep the board's existing 20% steps. The estimator expects descending voltage. */
    for (size_t i = 0; i < point_count; ++i) {
        const size_t raw_index = point_count - 1 - i;
        int adc_mv;
        const esp_err_t err = adc_cali_raw_to_voltage(calibration, s_battery_raw_at_percent[raw_index], &adc_mv);
        if (err != ESP_OK) {
            (void)adc_cali_delete_scheme_curve_fitting(calibration);
            return err;
        }
        s_battery_curve[i] = (battery_point_t){
            .voltage = adc_mv * battery_volts_per_adc_mv,
            .capacity = raw_index * 20,
        };
    }

    adc_battery_estimation_t config = {
        .external = {
            .adc_handle = s_adc,
            .adc_cali_handle = calibration,
        },
        .adc_channel = ADC_BATTERY,
        .upper_resistor = BATTERY_DIVIDER_UPPER_OHMS,
        .lower_resistor = BATTERY_DIVIDER_LOWER_OHMS,
        .battery_points = s_battery_curve,
        .battery_points_count = point_count,
        .charging_detect_cb = battery_is_charging,
    };
    s_battery_estimator = adc_battery_estimation_create(&config);
    if (!s_battery_estimator) {
        (void)adc_cali_delete_scheme_curve_fitting(calibration);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "calibrated battery estimator ready");
    return ESP_OK;
}

const char *board_hardware_name(void)
{
    return "magiclick_2p5";
}

size_t board_buttons(const board_button_t **buttons)
{
    *buttons = s_buttons;
    return sizeof(s_buttons) / sizeof(s_buttons[0]);
}

bool magiclick_2p5_has_gc9107(void)
{
    return s_gc9107;
}

esp_err_t board_init(void)
{
    /* Speaker amp and optional 4G module are active high. RGB LEDs are active low. */
    const gpio_num_t amp_enable = GPIO_NUM_4;
    const gpio_num_t modem_enable = GPIO_NUM_40;
    const gpio_num_t led_power = GPIO_NUM_39;
    ESP_RETURN_ON_ERROR(gpio_set_level(amp_enable, 0), TAG, "mute speaker");
    ESP_RETURN_ON_ERROR(gpio_set_level(modem_enable, 0), TAG, "disable modem");
    ESP_RETURN_ON_ERROR(gpio_set_level(led_power, 1), TAG, "disable LED power");
    const gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << amp_enable) | (1ULL << modem_enable) | (1ULL << led_power),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "configure peripheral enables");
    const gpio_config_t charger = {
        .pin_bit_mask = 1ULL << PIN_CHARGING,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&charger), TAG, "configure CHRG");

    const adc_oneshot_unit_init_cfg_t adc = {.unit_id = ADC_UNIT_1};
    const adc_oneshot_chan_cfg_t channel = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc, &s_adc), TAG, "initialize ADC");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, ADC_PCB_REVISION, &channel),
                        TAG, "configure PCB revision ADC");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, ADC_BATTERY, &channel),
                        TAG, "configure battery ADC");

    int revision;
    ESP_RETURN_ON_ERROR(read_adc_average(ADC_PCB_REVISION, &revision), TAG, "detect PCB revision");
    /* 2.5A straps GPIO3 low; 2.5A1 straps it high. Match upstream's 100 mV cutoff. */
    s_gc9107 = revision * 3300 < 100 * 4095;
    ESP_LOGI(TAG, "PCB strap raw=%d, using %s panel", revision,
             s_gc9107 ? "GC9107 (GC9A01 driver)" : "ST7735");
    const esp_err_t battery_err = init_battery_estimator(&channel);
    if (battery_err != ESP_OK) {
        ESP_LOGW(TAG, "battery calibration unavailable, using raw curve: %s", esp_err_to_name(battery_err));
    }

    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_5,
        .scl_io_num = GPIO_NUM_6,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &s_codec_bus), TAG, "initialize I2C");
    s_confirmation_motion = magiclick_2p5_init_shake_sensor(s_codec_bus);
    return ESP_OK;
}

const board_confirmation_motion_t *board_confirmation_motion(void)
{
    return s_confirmation_motion;
}

es8311_input_board_t board_es8311_input_config(void)
{
    return (es8311_input_board_t){
        .i2c_port = I2C_NUM_0,
        .bus = s_codec_bus,
        .i2s_port = I2S_NUM_0,
        .pins = {
            .mclk = GPIO_NUM_8,
            .bclk = GPIO_NUM_9,
            .ws = GPIO_NUM_11,
            .dout = I2S_GPIO_UNUSED,
            .din = GPIO_NUM_10,
        },
        .gain_db = 30.0f,
    };
}

static void IRAM_ATTR charging_isr(void *arg)
{
    (void)arg;
    s_power_event_cb();
}

esp_err_t board_power_events_init(board_power_event_cb_t callback)
{
    s_power_event_cb = callback;
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "install charger ISR service");
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_CHARGING, charging_isr, NULL),
                        TAG, "add charger ISR");
    return gpio_set_intr_type(PIN_CHARGING, GPIO_INTR_ANYEDGE);
}

void board_power_events_rearm(void)
{
    /* Both edges stay enabled. */
}

esp_err_t board_battery_level(int *level_percent)
{
    if (s_battery_estimator) {
        float capacity;
        ESP_RETURN_ON_ERROR(adc_battery_estimation_get_capacity(s_battery_estimator, &capacity),
                            TAG, "estimate battery capacity");
        *level_percent = (int)(capacity + 0.5f);
        return ESP_OK;
    }

    int raw;
    ESP_RETURN_ON_ERROR(read_adc_average(ADC_BATTERY, &raw), TAG, "sample battery");
    if (raw <= s_battery_raw_at_percent[0]) {
        *level_percent = 0;
        return ESP_OK;
    }
    for (size_t i = 1; i < sizeof(s_battery_raw_at_percent) / sizeof(s_battery_raw_at_percent[0]); i++) {
        if (raw < s_battery_raw_at_percent[i]) {
            const int base = s_battery_raw_at_percent[i - 1];
            *level_percent = ((int)i - 1) * 20 + (raw - base) * 20 / (s_battery_raw_at_percent[i] - base);
            return ESP_OK;
        }
    }
    *level_percent = 100;
    return ESP_OK;
}

esp_err_t board_battery_charging(bool *charging)
{
    *charging = battery_is_charging(NULL);
    return ESP_OK;
}

esp_err_t board_usb_powered(bool *usb_powered)
{
    /* CHRG covers active charging; USB Serial/JTAG detects a USB host. */
    *usb_powered = usb_serial_jtag_is_connected();
    return ESP_OK;
}

bool board_battery_critical(void)
{
    /* The board exposes CHRG and USB data, with no USB insertion wake pin. */
    return false;
}

esp_err_t board_prepare_deep_sleep(bool usb_wake_only)
{
    if (usb_wake_only) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (gpio_get_level(PIN_KEY_MAIN) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_dis(PIN_KEY_MAIN), TAG, "disable wake pin pulldown");
    ESP_RETURN_ON_ERROR(rtc_gpio_pullup_en(PIN_KEY_MAIN), TAG, "enable wake pin pullup");
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    return esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_KEY_MAIN, ESP_EXT1_WAKEUP_ANY_LOW);
}
