#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "sdkconfig.h"

/* GPIO map of hardware/voicestick_c3 (see GPIO_MAP in hardware/voicestick_c3/tools/design.py). */
#define VS_PIN_PERIPH_EN  GPIO_NUM_2   /* low = LED and mic rail on, 10k pull-up */
#define VS_PIN_BAT_ADC    GPIO_NUM_3   /* VBAT through 1M/1M divider */
#define VS_PIN_KEY_MAIN   GPIO_NUM_4   /* active low, 100k pull-up, deep-sleep wake */
#define VS_PIN_VBUS_SENSE GPIO_NUM_5   /* high = USB present, 100k/200k divider */
#define VS_PIN_PDM_CLK    GPIO_NUM_6
#define VS_PIN_PDM_DAT    GPIO_NUM_7
#define VS_PIN_KEY_PREV   GPIO_NUM_8   /* active low, 10k pull-up */
#define VS_PIN_KEY_CANCEL GPIO_NUM_9   /* active low, 10k pull-up, BOOT strap */
#define VS_PIN_CHRG       GPIO_NUM_10  /* TP4054 open drain, low = charging */
#define VS_PIN_LED_DIN    GPIO_NUM_20  /* WS2812B on the peripheral rail */
#define VS_PIN_KEY_NEXT   GPIO_NUM_21  /* active low, internal pull-up only */

/* The peripheral rail is shared by the status LED and the microphone and is
 * switched off when neither needs it. Returns true when this call powered it
 * up, in which case the caller must allow the rail to settle. */
bool voicestick_periph_power_acquire(void);
void voicestick_periph_power_release(void);

#ifdef CONFIG_VOICE_STICK_PDM_CAPTURE
void voicestick_pdm_capture_start(void);
#endif
