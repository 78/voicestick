#include "ui_status.h"

#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"

#include "voicestick_c3.h"

static const char *TAG = "status_led";

/* Channel value at the app's active brightness; the light pipe is small. */
#define LED_MAX_LEVEL 8
#define ACTIVE_BRIGHTNESS 128
#define LOW_BATTERY_PERCENT 10
#define ERROR_BLINK_US (3 * 1000 * 1000)
#define RAIL_SETTLE_US 1000

typedef enum {
    SCENE_BOOT,
    SCENE_PAIRING,
    SCENE_READY,
    SCENE_RECORDING,
    SCENE_THINKING,
    SCENE_PENDING,
    SCENE_OTA,
    SCENE_REBOOTING,
    SCENE_ERROR,
} scene_t;

typedef struct {
    uint8_t r, g, b;
    uint16_t on_ms;
    uint16_t period_ms; /* 0 and on_ms 0 = steady; 0 and on_ms > 0 = one pulse */
} pattern_t;

static const pattern_t s_patterns[] = {
    [SCENE_BOOT] = {255, 255, 255, 0, 0},
    [SCENE_PAIRING] = {0, 64, 255, 100, 2000},
    [SCENE_READY] = {0, 255, 32, 250, 0},
    [SCENE_RECORDING] = {255, 0, 0, 0, 0},
    [SCENE_THINKING] = {160, 0, 255, 500, 1000},
    [SCENE_PENDING] = {255, 160, 0, 0, 0},
    [SCENE_OTA] = {0, 200, 255, 250, 500},
    [SCENE_REBOOTING] = {255, 255, 255, 0, 0},
    [SCENE_ERROR] = {255, 0, 0, 150, 300},
};

static const pattern_t s_low_battery = {255, 80, 0, 0, 0};

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_blink_timer;
static scene_t s_scene = SCENE_BOOT;
static int64_t s_scene_started_us;
static uint8_t s_brightness = ACTIVE_BRIGHTNESS;
static bool s_idle_dimmed;
static bool s_low_battery_shown;
static bool s_blink_on;
static bool s_rendered;
static bool s_periph_powered;

static esp_err_t write_led(uint8_t r, uint8_t g, uint8_t b)
{
    const bool black = (r | g | b) == 0;
    if (black && !s_periph_powered) {
        return ESP_OK;
    }
    if (!s_periph_powered) {
        s_periph_powered = true;
        if (voicestick_periph_power_acquire()) {
            esp_rom_delay_us(RAIL_SETTLE_US);
        }
    }

    /* An enabled RMT channel holds a PM lock that blocks light sleep, so the
     * driver only exists for the duration of one refresh. */
    const led_strip_config_t strip_config = {
        .strip_gpio_num = VS_PIN_LED_DIN,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    led_strip_handle_t strip = NULL;
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (err == ESP_OK) {
        err = led_strip_set_pixel(strip, 0, r, g, b);
        if (err == ESP_OK) {
            err = led_strip_refresh(strip);
        }
        led_strip_del(strip);
    }

    /* Idle low between frames, light sleep included; WS2812 latches the last
     * color. */
    gpio_set_direction(VS_PIN_LED_DIN, GPIO_MODE_OUTPUT);
    gpio_set_level(VS_PIN_LED_DIN, 0);
    gpio_sleep_sel_dis(VS_PIN_LED_DIN);

    if (black) {
        voicestick_periph_power_release();
        s_periph_powered = false;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED write failed: %s", esp_err_to_name(err));
    }
    return err;
}

static const pattern_t *current_pattern(void)
{
    if (s_scene == SCENE_READY && s_low_battery_shown) {
        return &s_low_battery;
    }
    if (s_scene == SCENE_ERROR && esp_timer_get_time() - s_scene_started_us > ERROR_BLINK_US) {
        static const pattern_t steady_error = {255, 0, 0, 0, 0};
        return &steady_error;
    }
    return &s_patterns[s_scene];
}

static uint8_t scale(uint8_t level)
{
    return (uint8_t)((uint32_t)level * s_brightness * LED_MAX_LEVEL / (255u * ACTIVE_BRIGHTNESS));
}

static bool pattern_oneshot(const pattern_t *pattern)
{
    return pattern->period_ms == 0 && pattern->on_ms != 0;
}

/* Caller holds s_lock. */
static void render(void)
{
    const pattern_t *p = current_pattern();
    const bool oneshot = pattern_oneshot(p);
    const bool off = s_brightness == 0 || (s_scene == SCENE_READY && s_idle_dimmed);
    if (off && oneshot) {
        s_blink_on = false;
    }
    const bool lit = !off && (oneshot ? s_blink_on : (p->period_ms == 0 || s_blink_on));

    (void)esp_timer_stop(s_blink_timer);
    if (lit) {
        write_led(scale(p->r), scale(p->g), scale(p->b));
    } else {
        write_led(0, 0, 0);
    }
    if (!off && (p->period_ms != 0 || (oneshot && s_blink_on))) {
        const uint32_t ms = s_blink_on ? p->on_ms : (uint32_t)(p->period_ms - p->on_ms);
        (void)esp_timer_start_once(s_blink_timer, ms * 1000ULL);
    }
}

static void blink_timer_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_blink_on = !s_blink_on;
    render();
    xSemaphoreGive(s_lock);
}

static void set_scene(scene_t scene)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Repeated calls (OTA progress) must not restart the blink phase. */
    if (scene != s_scene || scene == SCENE_ERROR || !s_rendered) {
        s_scene = scene;
        s_scene_started_us = esp_timer_get_time();
        s_blink_on = true;
        s_rendered = true;
        render();
    }
    xSemaphoreGive(s_lock);
}

esp_err_t ui_status_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = blink_timer_cb,
        .name = "status_led",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_blink_timer);
    if (err != ESP_OK) {
        return err;
    }
    set_scene(SCENE_BOOT);
    return ESP_OK;
}

esp_err_t ui_status_set_brightness(uint8_t brightness)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_brightness = brightness;
    render();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void ui_status_prepare_deep_sleep(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    (void)esp_timer_stop(s_blink_timer);
    write_led(0, 0, 0);
    gpio_reset_pin(VS_PIN_LED_DIN);
    gpio_set_pull_mode(VS_PIN_LED_DIN, GPIO_FLOATING);
    xSemaphoreGive(s_lock);
}

void ui_status_set_device_name(const char *device_name)
{
    (void)device_name;
}

void ui_status_set_advertising(void)
{
}

void ui_status_set_pairing(const char *device_name)
{
    (void)device_name;
    set_scene(SCENE_PAIRING);
}

void ui_status_set_idle_hint(const char *hint)
{
    (void)hint;
}

void ui_status_set_idle(void)
{
    set_scene(SCENE_READY);
}

void ui_status_set_idle_dimmed(bool dimmed)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_idle_dimmed = dimmed;
    render();
    xSemaphoreGive(s_lock);
}

void ui_status_set_recording(uint32_t session_id)
{
    (void)session_id;
    set_scene(SCENE_RECORDING);
}

void ui_status_set_battery(int level_percent, bool charging, bool usb_powered)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool low = level_percent <= LOW_BATTERY_PERCENT && !charging && !usb_powered;
    if (low != s_low_battery_shown) {
        s_low_battery_shown = low;
        render();
    }
    xSemaphoreGive(s_lock);
}

void ui_status_set_partial_text(const char *text)
{
    /* The app sends "" while thinking and a prompt when awaiting confirmation. */
    set_scene(text && text[0] ? SCENE_PENDING : SCENE_THINKING);
}

void ui_status_set_ota_progress(uint32_t written, uint32_t size)
{
    (void)written;
    (void)size;
    set_scene(SCENE_OTA);
}

void ui_status_set_ota_rebooting(void)
{
    set_scene(SCENE_REBOOTING);
}

void ui_status_set_error(const char *message)
{
    ESP_LOGW(TAG, "error: %s", message ? message : "");
    set_scene(SCENE_ERROR);
}
