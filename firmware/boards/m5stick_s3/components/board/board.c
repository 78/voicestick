#include "board.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "stick_s3_board.h"

static const char *TAG = "board";

static const board_button_t s_buttons[] = {
    {STICK_S3_PIN_BUTTON_FRONT, "primary"},
    {STICK_S3_PIN_BUTTON_SIDE, "secondary"},
};

static board_power_event_cb_t s_power_event_cb;

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
    return stick_s3_board_init();
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
