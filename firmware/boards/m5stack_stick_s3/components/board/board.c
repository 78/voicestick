#include "board.h"

#include "bmi270.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_lcd.h"
#include "es8311_input_board.h"
#include "stick_s3_board.h"

static const char *TAG = "board";

static const board_button_t s_buttons[] = {
    {STICK_S3_PIN_BUTTON_FRONT, "primary"},
    {STICK_S3_PIN_BUTTON_SIDE, "secondary"},
};

static board_power_event_cb_t s_power_event_cb;
static bmi270_handle_t *s_imu_sensor;

#define BMI270_REG_PWR_CONF 0x7c
#define BMI270_REG_PWR_CTRL 0x7d
#define BMI270_ADV_POWER_SAVE_AND_FIFO_SELF_WAKEUP 0x03
#define BMI270_GYRO_ENABLE BIT(1)
#define BMI270_ACCEL_ENABLE BIT(2)
#define BMI270_IO_TIMEOUT_MS 20

static bool set_imu_enabled(bool enabled, bool include_gyro, uint16_t rate_hz)
{
    if (!s_imu_sensor) {
        return false;
    }
    if (!enabled) {
        const uint8_t power_off[] = {BMI270_REG_PWR_CTRL, 0};
        const uint8_t power_save[] = {BMI270_REG_PWR_CONF, BMI270_ADV_POWER_SAVE_AND_FIFO_SELF_WAKEUP};
        (void)i2c_master_transmit(s_imu_sensor->i2c_handle, power_off, sizeof(power_off), BMI270_IO_TIMEOUT_MS);
        esp_rom_delay_us(450);
        if (i2c_master_transmit(s_imu_sensor->i2c_handle, power_save, sizeof(power_save), BMI270_IO_TIMEOUT_MS) != ESP_OK) {
            return false;
        }
        esp_rom_delay_us(450);
        return true;
    }

    const uint8_t power_active[] = {BMI270_REG_PWR_CONF, 0};
    if (i2c_master_transmit(s_imu_sensor->i2c_handle, power_active, sizeof(power_active), BMI270_IO_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    esp_rom_delay_us(450);

    const uint8_t acc_odr = rate_hz <= 50 ? BMI270_ACC_ODR_50_HZ : BMI270_ACC_ODR_100_HZ;
    if (bmi270_set_acce_odr(s_imu_sensor, acc_odr) != ESP_OK ||
        bmi270_set_acce_range(s_imu_sensor, BMI270_ACC_RANGE_4_G) != ESP_OK) {
        return false;
    }
    if (include_gyro) {
        const uint8_t gyr_odr = rate_hz <= 50 ? BMI270_GYR_ODR_50_HZ : BMI270_GYR_ODR_100_HZ;
        if (bmi270_set_gyro_odr(s_imu_sensor, gyr_odr) != ESP_OK ||
            bmi270_set_gyro_range(s_imu_sensor, BMI270_GYR_RANGE_2000_DPS) != ESP_OK) {
            return false;
        }
    }

    const uint8_t power[] = {BMI270_REG_PWR_CTRL,
                             (uint8_t)(BMI270_ACCEL_ENABLE | (include_gyro ? BMI270_GYRO_ENABLE : 0))};
    return i2c_master_transmit(s_imu_sensor->i2c_handle, power, sizeof(power), BMI270_IO_TIMEOUT_MS) == ESP_OK;
}

static bool read_imu_samples(bool include_gyro, uint8_t *out_bytes)
{
    const uint8_t acc_x_lsb = 0x0c;
    const size_t read_len = include_gyro ? 12 : 6;
    return i2c_master_transmit_receive(s_imu_sensor->i2c_handle, &acc_x_lsb, 1, out_bytes, read_len,
                                       BMI270_IO_TIMEOUT_MS) == ESP_OK;
}

const board_imu_t *board_imu(void)
{
    static const board_imu_t imu = {
        .has_gyro = true,
        .accel_lsb_per_g = 8192,
        .gyro_lsb_per_dps = 16.384f,
        .max_rate_hz = 100,
        .set_enabled = set_imu_enabled,
        .read_samples = read_imu_samples,
    };
    return s_imu_sensor ? &imu : NULL;
}

static esp_err_t init_imu_sensor(i2c_master_bus_handle_t bus)
{
    const bmi270_driver_config_t config = {
        .addr = BMI270_I2C_ADDRESS_L,
        .interface = BMI270_USE_I2C,
        .i2c_bus = bus,
    };
    bmi270_handle_t *sensor;
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, config.addr, BMI270_IO_TIMEOUT_MS), TAG, "probe BMI270");
    ESP_RETURN_ON_ERROR(bmi270_create(&config, &sensor), TAG, "initialize BMI270");

    esp_err_t ret;
    ESP_GOTO_ON_ERROR(bmi270_set_acce_odr(sensor, BMI270_ACC_ODR_100_HZ), cleanup, TAG, "set BMI270 sample rate");
    ESP_GOTO_ON_ERROR(bmi270_set_acce_range(sensor, BMI270_ACC_RANGE_4_G), cleanup, TAG, "set BMI270 range");
    ESP_GOTO_ON_ERROR(bmi270_stop(sensor), cleanup, TAG, "put BMI270 sensors in standby");
    const uint8_t power_save[] = {BMI270_REG_PWR_CONF, BMI270_ADV_POWER_SAVE_AND_FIFO_SELF_WAKEUP};
    ESP_GOTO_ON_ERROR(i2c_master_transmit(sensor->i2c_handle, power_save, sizeof(power_save), BMI270_IO_TIMEOUT_MS),
                      cleanup, TAG, "enable BMI270 power save");
    s_imu_sensor = sensor;
    ESP_LOGI(TAG, "BMI270 6-axis IMU ready");
    return ESP_OK;

cleanup:
    (void)bmi270_delete(sensor);
    return ret;
}

const char *board_hardware_name(void)
{
    return "stick_s3";
}

size_t board_buttons(const board_button_t **buttons)
{
    *buttons = s_buttons;
    return sizeof(s_buttons) / sizeof(s_buttons[0]);
}

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(stick_s3_board_init(), TAG, "initialize StickS3 board");
    i2c_master_bus_handle_t bus = stick_s3_board_i2c_bus();
    if (bus) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(init_imu_sensor(bus));
    }
    return ESP_OK;
}

es8311_input_board_t board_es8311_input_config(void)
{
    return (es8311_input_board_t){
        .i2c_port = STICK_S3_I2C_PORT,
        .bus = stick_s3_board_i2c_bus(),
        .i2s_port = I2S_NUM_1,
        .pins = {
            .mclk = STICK_S3_PIN_ES8311_MCLK,
            .bclk = STICK_S3_PIN_ES8311_BCLK,
            .ws = STICK_S3_PIN_ES8311_LRCK,
            .dout = STICK_S3_PIN_ES8311_DIN,
            .din = STICK_S3_PIN_ES8311_DOUT,
        },
        .gain_db = 36.0f,
    };
}

static esp_err_t new_st7789(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t *panel)
{
    const esp_lcd_panel_dev_config_t config = {
        .reset_gpio_num = STICK_S3_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    return esp_lcd_new_panel_st7789(io, &config, panel);
}

board_lcd_t board_lcd_config(void)
{
    return (board_lcd_t){
        .host = SPI2_HOST,
        .sck = STICK_S3_PIN_LCD_SCK,
        .mosi = STICK_S3_PIN_LCD_MOSI,
        .cs = STICK_S3_PIN_LCD_CS,
        .dc = STICK_S3_PIN_LCD_DC,
        .backlight = STICK_S3_PIN_LCD_BL,
        .width = 135,
        .height = 240,
        .gap_x = 52,
        .gap_y = 40,
        .invert_colors = true,
        .new_panel = new_st7789,
    };
}

static void IRAM_ATTR pmic_irq_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(STICK_S3_PIN_PMIC_IRQ);
    if (s_power_event_cb) {
        s_power_event_cb();
    }
}

esp_err_t board_power_events_init(board_power_event_cb_t callback)
{
    s_power_event_cb = callback;

    gpio_config_t irq_config = {
        .pin_bit_mask = 1ULL << STICK_S3_PIN_PMIC_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&irq_config);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_wakeup_enable(STICK_S3_PIN_PMIC_IRQ, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(STICK_S3_PIN_PMIC_IRQ, pmic_irq_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_intr_type(STICK_S3_PIN_PMIC_IRQ, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_intr_enable(STICK_S3_PIN_PMIC_IRQ);
}

void board_power_events_rearm(void)
{
    uint8_t sys_status = 0;
    esp_err_t err = stick_s3_board_clear_power_irqs(&sys_status);
    if (err == ESP_OK && sys_status) {
        ESP_LOGI(TAG, "PMIC sys irq=0x%02x", sys_status);
    }
    gpio_intr_enable(STICK_S3_PIN_PMIC_IRQ);
}

esp_err_t board_battery_level(int *level_percent)
{
    return stick_s3_board_battery_level(level_percent);
}

esp_err_t board_battery_charging(bool *charging)
{
    return stick_s3_board_battery_charging(charging);
}

esp_err_t board_usb_powered(bool *usb_powered)
{
    return stick_s3_board_usb_powered(usb_powered);
}

bool board_battery_critical(void)
{
    /* The M5PM1 PMIC cuts power itself. */
    return false;
}

esp_err_t board_prepare_deep_sleep(bool usb_wake_only)
{
    /* The PMIC handles USB insertion; the front button always wakes. */
    (void)usb_wake_only;
    const gpio_num_t wake_gpio = STICK_S3_PIN_BUTTON_FRONT;
    if (!esp_sleep_is_valid_wakeup_gpio(wake_gpio)) {
        ESP_LOGE(TAG, "GPIO%d cannot wake from deep sleep", wake_gpio);
        return ESP_ERR_NOT_SUPPORTED;
    }

    stick_s3_board_prepare_deep_sleep();

    /* Clear any stale wakeup source bits left over from light sleep / esp_pm
       configuration (e.g. gpio_wakeup_enable on the PMIC IRQ line). Without
       this the chip can wake immediately from an unrelated trigger. */
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    /* Keep RTC peripherals powered so the internal pull-up on the wake pin
       remains effective in deep sleep; combined with the explicit RTC pull-up
       below this prevents the pin from floating low and self-waking. */
    (void)esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    (void)rtc_gpio_pulldown_dis(wake_gpio);
    (void)rtc_gpio_pullup_en(wake_gpio);

    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << wake_gpio,
                                                    ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable deep sleep wake failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Wait for the wake pin to settle high (button release bounce, parasitic
       capacitance, etc.). If it stays low we would just wake up immediately
       after esp_deep_sleep_start(), so abort and retry later. */
    int wait_ms = 0;
    while (gpio_get_level(wake_gpio) == 0 && wait_ms < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (gpio_get_level(wake_gpio) == 0) {
        ESP_LOGW(TAG, "front button still low after %d ms, abort deep sleep", wait_ms);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "deep sleep armed (wait_ms=%d), wake on front button GPIO%d low",
             wait_ms, wake_gpio);
    return ESP_OK;
}
