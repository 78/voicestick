#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Board microphone, opened for each recording session. */
#define AUDIO_INPUT_SAMPLE_RATE 16000

esp_err_t audio_input_open(void);
/* Blocks until `count` mono PCM16 samples have been read. */
esp_err_t audio_input_read(int16_t *samples, size_t count);
void audio_input_close(void);
