#pragma once

/*
 * Die Board-Schicht: was ein Zielgeraet ausmacht, unter einem Namen, der fuer
 * beide gilt.
 *
 * Es gibt zwei Geraete:
 *
 *   StickS3   M5Stack StickS3, ST7789 240x135, ES8311 fuer Mikrofon und
 *             Lautsprecher, M5PM1 als Leistungsbaustein. Der Ursprung.
 *   Watch206  Waveshare ESP32-S3-Touch-AMOLED-2.06, CO5300-AMOLED 410x502
 *             hochkant, ES7210 fuer die zwei Mikrofone, ES8311 nur fuer den
 *             Lautsprecher, AXP2101, PCF85063-Uhr, FT3168-Touch, Motor.
 *
 * Gewaehlt wird beim Bauen (`-DVOICE_BOARD=watch206`, siehe
 * firmware/CMakeLists.txt), nicht zur Laufzeit: Pins, Treiber und Layout
 * unterscheiden sich so weit, dass ein gemeinsames Abbild nur toten Code
 * mitschleppen wuerde.
 *
 * Fuer den StickS3 reicht dieser Header die Funktionen aus stick_s3_board nur
 * durch. Die Komponente bleibt dadurch so, wie sie ist -- weniger Reibung beim
 * naechsten Abgleich mit dem Upstream.
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "sdkconfig.h"

/* Was der Finger auf dem Schirm getan hat. */
typedef enum {
    VOICE_BOARD_TAP,
    VOICE_BOARD_SWIPE_LEFT,
    VOICE_BOARD_SWIPE_RIGHT,
    VOICE_BOARD_SWIPE_UP,
    VOICE_BOARD_SWIPE_DOWN,
    /* Flache Hand auf dem Schirm: alles still. */
    VOICE_BOARD_PALM,
    /* Ein Finger 0,6 bis 2 s ohne Wischen (0.4.4): auf dem Zifferblatt die Layout-Auswahl. */
    VOICE_BOARD_LONG_PRESS,
} voice_board_gesture_t;

typedef struct {
    voice_board_gesture_t gesture;
    /* Wo der Finger aufsetzte, in Display-Koordinaten (wie LVGL sie sieht). */
    int16_t x;
    int16_t y;
} voice_board_touch_event_t;

/* Wird vom Touch-Baustein aus einer eigenen Task gerufen, nie aus einer ISR. */
typedef void (*voice_board_touch_cb_t)(const voice_board_touch_event_t *event);

#if CONFIG_VOICE_BOARD_WATCH206

/* Steht im device_info-Ereignis (docs/protocol.md) -- daran erkennt die
   Bruecke, welches Abbild auf dieses Geraet gehoert. */
#define VOICE_BOARD_HARDWARE_ID "watch_206"

/*
 * Die Tasten der Uhr, und warum die Rollen so verteilt sind.
 *
 * BOOT (GPIO0) haengt mit 10 kOhm an 3,3 V und schaltet gegen Masse: aktiv
 * low, wie beim StickS3.
 *
 * PWR geht nicht direkt an den Chip, sondern an den PWRON-Eingang des AXP2101.
 * Ein BSS138 kehrt den Pegel um und legt ihn auf GPIO10 (Netz SYS_OUT im
 * Schaltplan): **gedrueckt ist high**.
 *
 * Gesprochen wird auf BOOT, geblaettert auf PWR. Umgekehrt ginge es nicht:
 * Der AXP2101 schaltet ab, wenn PWRON lange genug unten liegt (ab Werk sechs
 * Sekunden, hoechstens zehn) -- und eine Aufnahme darf dreissig dauern. Die
 * Blaettertaste wird nur kurz gedrueckt; dass sie lange gehalten das Geraet
 * ausschaltet, ist dort sogar der Notausgang, wenn die Firmware haengt.
 */
#define VOICE_BOARD_PIN_BUTTON_TALK   GPIO_NUM_0
#define VOICE_BOARD_PIN_BUTTON_BROWSE GPIO_NUM_10
#define VOICE_BOARD_BUTTON_TALK_ACTIVE_LEVEL   0
#define VOICE_BOARD_BUTTON_BROWSE_ACTIVE_LEVEL 1

/* Die IRQ-Leitung des AXP2101 erreicht den Chip nicht (Netz EXIO5, frei). */
#define VOICE_BOARD_PIN_PMIC_IRQ GPIO_NUM_NC

#define VOICE_BOARD_PIN_I2C_SCL GPIO_NUM_14
#define VOICE_BOARD_PIN_I2C_SDA GPIO_NUM_15

/* I2S aus Sicht des Chips: DOUT geht zum Lautsprecher, DIN kommt vom ES7210. */
#define VOICE_BOARD_PIN_I2S_MCLK GPIO_NUM_16
#define VOICE_BOARD_PIN_I2S_BCLK GPIO_NUM_41
#define VOICE_BOARD_PIN_I2S_WS   GPIO_NUM_45
#define VOICE_BOARD_PIN_I2S_DOUT GPIO_NUM_40
#define VOICE_BOARD_PIN_I2S_DIN  GPIO_NUM_42
/* Verstaerker vor dem Lautsprecher (Netz PA_CTRL). */
#define VOICE_BOARD_PIN_PA       46
/* Mikrofone haengen am ES7210, nicht am ES8311 wie beim StickS3. */
#define VOICE_BOARD_MIC_ES7210 1

#define VOICE_BOARD_PIN_LCD_CS    GPIO_NUM_12
#define VOICE_BOARD_PIN_LCD_PCLK  GPIO_NUM_11
#define VOICE_BOARD_PIN_LCD_DATA0 GPIO_NUM_4
#define VOICE_BOARD_PIN_LCD_DATA1 GPIO_NUM_5
#define VOICE_BOARD_PIN_LCD_DATA2 GPIO_NUM_6
#define VOICE_BOARD_PIN_LCD_DATA3 GPIO_NUM_7
#define VOICE_BOARD_PIN_LCD_RST   GPIO_NUM_8

#define VOICE_BOARD_PIN_TOUCH_RST GPIO_NUM_9
#define VOICE_BOARD_PIN_TOUCH_INT GPIO_NUM_38

/* Basis des Motortransistors (Netz MOTOR). */
#define VOICE_BOARD_PIN_MOTOR GPIO_NUM_18

esp_err_t voice_board_init(void);
i2c_master_bus_handle_t voice_board_i2c_bus(void);
esp_err_t voice_board_battery_level(int *level_percent);
esp_err_t voice_board_battery_charging(bool *charging);
esp_err_t voice_board_usb_powered(bool *usb_powered);
esp_err_t voice_board_clear_power_irqs(uint8_t *sys_status);
void voice_board_prepare_deep_sleep(void);
bool voice_board_talk_button_pressed(void);
bool voice_board_browse_button_pressed(void);

/*
 * Die Uhr im PCF85063. Sie laeuft am Akku weiter, auch im Tiefschlaf und
 * nach einem Neustart -- die Uhrzeit steht also, bevor eine Bruecke da ist.
 * `epoch` ist UTC; der Abstand zur Ortszeit liegt im einen RAM-Byte des
 * Bausteins, in Viertelstunden. ESP_ERR_INVALID_STATE heisst: Der Baustein
 * hat nie eine Zeit bekommen oder war ohne Strom.
 */
esp_err_t voice_board_rtc_read(int64_t *epoch, int32_t *tz_offset_min);
esp_err_t voice_board_rtc_write(int64_t epoch, int32_t tz_offset_min);

/* Ein kurzer Stoss des Vibrationsmotors; kehrt sofort zurueck. */
void voice_board_haptic(uint32_t duration_ms);
/* `count` Stoesse zu `on_ms`, dazwischen `off_ms` Pause; kehrt sofort zurueck. */
void voice_board_haptic_pattern(uint8_t count, uint32_t on_ms, uint32_t off_ms);

/* Tippen und Wischen auf dem Schirm, ausgewertet beim Abheben des Fingers. */
esp_err_t voice_board_touch_init(voice_board_touch_cb_t cb);

#else /* StickS3 */

#include "stick_s3_board.h"

#define VOICE_BOARD_HARDWARE_ID "stick_s3"

#define VOICE_BOARD_PIN_BUTTON_TALK   STICK_S3_PIN_BUTTON_TALK
#define VOICE_BOARD_PIN_BUTTON_BROWSE STICK_S3_PIN_BUTTON_BROWSE
#define VOICE_BOARD_BUTTON_TALK_ACTIVE_LEVEL   0
#define VOICE_BOARD_BUTTON_BROWSE_ACTIVE_LEVEL 0
#define VOICE_BOARD_PIN_PMIC_IRQ STICK_S3_PIN_PMIC_IRQ

#define VOICE_BOARD_PIN_I2S_MCLK STICK_S3_PIN_ES8311_MCLK
#define VOICE_BOARD_PIN_I2S_BCLK STICK_S3_PIN_ES8311_BCLK
#define VOICE_BOARD_PIN_I2S_WS   STICK_S3_PIN_ES8311_LRCK
#define VOICE_BOARD_PIN_I2S_DOUT STICK_S3_PIN_ES8311_DIN
#define VOICE_BOARD_PIN_I2S_DIN  STICK_S3_PIN_ES8311_DOUT
#define VOICE_BOARD_PIN_PA       (-1)
#define VOICE_BOARD_MIC_ES7210 0

static inline esp_err_t voice_board_init(void) { return stick_s3_board_init(); }
static inline i2c_master_bus_handle_t voice_board_i2c_bus(void) { return stick_s3_board_i2c_bus(); }
static inline esp_err_t voice_board_battery_level(int *level_percent)
{
    return stick_s3_board_battery_level(level_percent);
}
static inline esp_err_t voice_board_battery_charging(bool *charging)
{
    return stick_s3_board_battery_charging(charging);
}
static inline esp_err_t voice_board_usb_powered(bool *usb_powered)
{
    return stick_s3_board_usb_powered(usb_powered);
}
static inline esp_err_t voice_board_clear_power_irqs(uint8_t *sys_status)
{
    return stick_s3_board_clear_power_irqs(sys_status);
}
static inline void voice_board_prepare_deep_sleep(void) { stick_s3_board_prepare_deep_sleep(); }
static inline bool voice_board_talk_button_pressed(void) { return stick_s3_talk_button_pressed(); }
static inline bool voice_board_browse_button_pressed(void) { return stick_s3_browse_button_pressed(); }

/* Der StickS3 hat weder Uhrbaustein noch Motor noch Touch. */
static inline esp_err_t voice_board_rtc_read(int64_t *epoch, int32_t *tz_offset_min)
{
    (void)epoch;
    (void)tz_offset_min;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t voice_board_rtc_write(int64_t epoch, int32_t tz_offset_min)
{
    (void)epoch;
    (void)tz_offset_min;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline void voice_board_haptic(uint32_t duration_ms) { (void)duration_ms; }
static inline void voice_board_haptic_pattern(uint8_t count, uint32_t on_ms, uint32_t off_ms)
{
    (void)count;
    (void)on_ms;
    (void)off_ms;
}
static inline esp_err_t voice_board_touch_init(voice_board_touch_cb_t cb)
{
    (void)cb;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

/*
 * Das Haptik-Vokabular (Paket J, 0.5.0): genau vier Muster, damit der Arm
 * lernt, was eine Vibration heisst, ohne auf den Schirm zu sehen. Jede
 * Rueckmeldung der Uhr ist eines davon -- vorher gab es rund zehn Laengen
 * zwischen 15 und 300 ms, und keine sagte etwas Bestimmtes.
 *
 *   Erfolg       2 x kurz    etwas ist angekommen oder ausgefuehrt
 *   Fehler       1 x lang    hat nicht geklappt, nichts gesendet
 *   Frage        1 x mittel  ein zweiter Schritt kommt (OK, Auswahl)
 *   Erinnerung   3 x kurz    Termin gleich, neue Warnung
 *
 * Die eine Ausnahme ist der Tick beim Start einer Aufnahme (60 ms, "jetzt
 * sprechen") -- er ist kein Ergebnis, sondern ein Startsignal. Die Tabelle
 * steht auch im README (Abschnitt der Uhr). Auf dem StickS3 ohne Motor tut
 * das nichts.
 */
typedef enum {
    VOICE_HAPTIC_SUCCESS,
    VOICE_HAPTIC_ERROR,
    VOICE_HAPTIC_ATTENTION,
    VOICE_HAPTIC_REMINDER,
    VOICE_HAPTIC_RECORD_TICK,
} voice_haptic_t;

#define VOICE_HAPTIC_SHORT_MS 50
#define VOICE_HAPTIC_GAP_MS 90
#define VOICE_HAPTIC_MEDIUM_MS 140
#define VOICE_HAPTIC_LONG_MS 320
#define VOICE_HAPTIC_TICK_MS 60

static inline void voice_board_haptic_kind(voice_haptic_t kind)
{
    switch (kind) {
    case VOICE_HAPTIC_SUCCESS:
        voice_board_haptic_pattern(2, VOICE_HAPTIC_SHORT_MS, VOICE_HAPTIC_GAP_MS);
        break;
    case VOICE_HAPTIC_ERROR:
        voice_board_haptic_pattern(1, VOICE_HAPTIC_LONG_MS, 0);
        break;
    case VOICE_HAPTIC_ATTENTION:
        voice_board_haptic_pattern(1, VOICE_HAPTIC_MEDIUM_MS, 0);
        break;
    case VOICE_HAPTIC_REMINDER:
        voice_board_haptic_pattern(3, VOICE_HAPTIC_SHORT_MS, VOICE_HAPTIC_GAP_MS);
        break;
    case VOICE_HAPTIC_RECORD_TICK:
        voice_board_haptic_pattern(1, VOICE_HAPTIC_TICK_MS, 0);
        break;
    }
}
