#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_start(uint16_t stream_id, uint32_t initial_seq);
esp_err_t audio_pipeline_stop(bool wait_for_exit);
bool audio_pipeline_is_running(void);
uint16_t audio_pipeline_stream_id(void);
uint32_t audio_pipeline_next_seq(void);
