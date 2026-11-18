#include "audio_input.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_pdm.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "voicestick_c3.h"

static const char *TAG = "audio_input";

/* The ESP32-C3 has no PDM-to-PCM hardware: I2S captures the raw bitstream
 * and a CIC3 decimator runs here. The SD18OB371 only accepts clocks inside
 * 1.38-1.7, 2.1-2.6 or 2.9-4.8 MHz, hence 16 kHz x 96 or x 192. */
#define PDM_DECIMATION CONFIG_VOICE_STICK_PDM_DECIMATION
#define PDM_CLOCK_HZ (AUDIO_INPUT_SAMPLE_RATE * PDM_DECIMATION)
#define PDM_WORDS_PER_SAMPLE (PDM_DECIMATION / 16)
/* CIC3 gain is R^3; this brings full scale back to +-32768. */
#define PDM_CIC_NORMALIZE ((PDM_DECIMATION * PDM_DECIMATION * PDM_DECIMATION) / 32768)

/* RAW mode always delivers left and right 16-bit words; the mic's L/R pin
 * is tied low, so its data is in one slot only. */
#define DMA_FRAME_BYTES 4
#define DMA_BUFFER_MS 5
#define DMA_BUFFER_COUNT 16
#define READ_CHUNK_MS 10
#define READ_CHUNK_SAMPLES (AUDIO_INPUT_SAMPLE_RATE * READ_CHUNK_MS / 1000)
#define READ_CHUNK_BYTES (READ_CHUNK_SAMPLES * PDM_WORDS_PER_SAMPLE * DMA_FRAME_BYTES)
#define MIC_STARTUP_MS 100

#ifdef CONFIG_VOICE_STICK_PDM_RIGHT_SLOT
#define PDM_SLOT_OFFSET 2
#else
#define PDM_SLOT_OFFSET 0
#endif

typedef struct {
    uint32_t integrator[3];
    uint32_t comb[3];
    unsigned phase;
    int32_t dc_x;
    int32_t dc_y;
    int32_t eq_history[2];
} pdm_decimator_t;

/* Per byte, MSB first, x in {-1,+1}: sum(x), sum((8-i)x), sum((8-i)(9-i)/2 x). */
static int16_t s_lut_s1[256];
static int16_t s_lut_s2[256];
static int16_t s_lut_s3[256];

/* Bring-up statistics, logged once per second while recording. */
typedef struct {
    uint64_t ones[2]; /* left, right */
    uint64_t bits;
    uint64_t sum_squares;
    uint32_t samples;
    int32_t peak;
} pdm_stats_t;

static i2s_chan_handle_t s_rx;
static uint8_t *s_chunk;
static pdm_decimator_t s_decimator;
static pdm_stats_t s_stats;
static bool s_periph_powered;

static void update_stats(const uint8_t *stereo, size_t bytes, const int16_t *pcm, size_t samples)
{
    for (size_t i = 0; i < bytes; i += DMA_FRAME_BYTES) {
        s_stats.ones[0] += __builtin_popcount(stereo[i] | (stereo[i + 1] << 8));
        s_stats.ones[1] += __builtin_popcount(stereo[i + 2] | (stereo[i + 3] << 8));
        s_stats.bits += 16;
    }
    for (size_t i = 0; i < samples; ++i) {
        const int32_t x = pcm[i];
        s_stats.sum_squares += (uint64_t)(x * x);
        if (x > s_stats.peak || -x > s_stats.peak) {
            s_stats.peak = x < 0 ? -x : x;
        }
    }
    s_stats.samples += samples;
    if (s_stats.samples < AUDIO_INPUT_SAMPLE_RATE) {
        return;
    }

    /* A live PDM slot sits near 50% ones; a floating or stuck one does not. */
    const unsigned rms = (unsigned)sqrtf((float)(s_stats.sum_squares / s_stats.samples));
    ESP_LOGI(TAG, "ones L=%u%% R=%u%% pcm peak=%d rms=%u",
             (unsigned)(s_stats.ones[0] * 100 / s_stats.bits),
             (unsigned)(s_stats.ones[1] * 100 / s_stats.bits),
             (int)s_stats.peak, rms);
    ESP_LOG_BUFFER_HEX(TAG, stereo, 32);
    s_stats = (pdm_stats_t){0};
}

static void init_luts(void)
{
    for (int b = 0; b < 256; ++b) {
        int s1 = 0, s2 = 0, s3 = 0;
        for (int i = 0; i < 8; ++i) {
            const int x = (b & (0x80 >> i)) ? 1 : -1;
            s1 += x;
            s2 += (8 - i) * x;
            s3 += (8 - i) * (9 - i) / 2 * x;
        }
        s_lut_s1[b] = (int16_t)s1;
        s_lut_s2[b] = (int16_t)s2;
        s_lut_s3[b] = (int16_t)s3;
    }
}

/* Eight CIC3 integrator steps at once. Wraps modulo 2^32 by design. */
static inline void integrate_byte(pdm_decimator_t *d, uint8_t b)
{
    const uint32_t a = d->integrator[0];
    const uint32_t c = d->integrator[1];
    d->integrator[2] += 8u * c + 36u * a + (uint32_t)s_lut_s3[b];
    d->integrator[1] += 8u * a + (uint32_t)s_lut_s2[b];
    d->integrator[0] += (uint32_t)s_lut_s1[b];
}

static int16_t decimate(pdm_decimator_t *d)
{
    uint32_t x = d->integrator[2];
    for (int i = 0; i < 3; ++i) {
        const uint32_t diff = x - d->comb[i];
        d->comb[i] = x;
        x = diff;
    }
    const int32_t cic = (x <= INT32_MAX ? (int32_t)x : -(int32_t)(~x) - 1) / PDM_CIC_NORMALIZE;

    /* DC blocker, ~40 Hz. */
    const int32_t dc = cic - d->dc_x + (int32_t)(((int64_t)32257 * d->dc_y) / 32768);
    d->dc_x = cic;
    d->dc_y = dc;

    /* 3-tap CIC droop compensation: +3.5 dB at 4 kHz, +5.9 dB at 7 kHz. */
    const int32_t eq = (6 * d->eq_history[0] - dc - d->eq_history[1]) / 4;
    d->eq_history[1] = d->eq_history[0];
    d->eq_history[0] = dc;

    const int32_t y = eq * CONFIG_VOICE_STICK_MIC_GAIN;
    if (y > INT16_MAX) {
        return INT16_MAX;
    }
    if (y < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)y;
}

static void decimate_chunk(const uint8_t *stereo, size_t samples, int16_t *out)
{
    pdm_decimator_t *d = &s_decimator;
    const uint8_t *word = stereo + PDM_SLOT_OFFSET;
    for (size_t n = 0; n < samples;) {
        /* Little-endian 16-bit words, earliest bit in the MSB. */
        integrate_byte(d, word[1]);
        integrate_byte(d, word[0]);
        word += DMA_FRAME_BYTES;
        if (++d->phase == PDM_WORDS_PER_SAMPLE) {
            d->phase = 0;
            out[n++] = decimate(d);
        }
    }
}

static esp_err_t read_chunk(size_t bytes)
{
    size_t offset = 0;
    while (offset < bytes) {
        size_t received = 0;
        esp_err_t err = i2s_channel_read(s_rx, s_chunk + offset, bytes - offset, &received, 1000);
        if (err != ESP_OK) {
            return err;
        }
        offset += received;
    }
    return ESP_OK;
}

static void release_pdm_pins(void)
{
    gpio_reset_pin(VS_PIN_PDM_CLK);
    gpio_reset_pin(VS_PIN_PDM_DAT);
    gpio_set_pull_mode(VS_PIN_PDM_CLK, GPIO_FLOATING);
    gpio_set_pull_mode(VS_PIN_PDM_DAT, GPIO_FLOATING);
}

void audio_input_close(void)
{
    if (s_rx) {
        (void)i2s_channel_disable(s_rx);
        (void)i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    release_pdm_pins();
    if (s_periph_powered) {
        voicestick_periph_power_release();
        s_periph_powered = false;
    }
    free(s_chunk);
    s_chunk = NULL;
}

esp_err_t audio_input_open(void)
{
    if (s_rx) {
        return ESP_OK;
    }
    static bool luts_ready;
    if (!luts_ready) {
        init_luts();
        luts_ready = true;
    }
    s_decimator = (pdm_decimator_t){0};
    s_stats = (pdm_stats_t){0};

    s_chunk = malloc(READ_CHUNK_BYTES);
    ESP_RETURN_ON_FALSE(s_chunk, ESP_ERR_NO_MEM, TAG, "no memory for PDM buffer");

    s_periph_powered = true;
    (void)voicestick_periph_power_acquire();

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = DMA_BUFFER_COUNT;
    channel.dma_frame_num = PDM_CLOCK_HZ / 16 * DMA_BUFFER_MS / 1000;
    esp_err_t err = i2s_new_channel(&channel, NULL, &s_rx);
    if (err == ESP_OK) {
        i2s_pdm_rx_config_t config = {
            /* In RAW mode the "sample rate" is the PDM bit clock. */
            .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(PDM_CLOCK_HZ),
            .slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                               I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .clk = VS_PIN_PDM_CLK,
                .din = VS_PIN_PDM_DAT,
            },
        };
#ifdef CONFIG_VOICE_STICK_PDM_INVERT_CLK
        config.gpio_cfg.invert_flags.clk_inv = true;
#endif
        err = i2s_channel_init_pdm_rx_mode(s_rx, &config);
    }
    if (err == ESP_OK) {
        /* The mic drives DATA for half a clock and floats it for the other
         * half; the pull-down makes the idle slot read all zeros. */
        err = gpio_pulldown_en(VS_PIN_PDM_DAT);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_rx);
    }

    /* Drop the microphone's start-up time, but run it through the decimator
     * so the CIC and DC blocker have settled on the mic's offset. */
    int16_t discard[READ_CHUNK_SAMPLES];
    for (int ms = 0; err == ESP_OK && ms < MIC_STARTUP_MS; ms += READ_CHUNK_MS) {
        err = read_chunk(READ_CHUNK_BYTES);
        if (err == ESP_OK) {
            decimate_chunk(s_chunk, READ_CHUNK_SAMPLES, discard);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PDM start failed: %s", esp_err_to_name(err));
        audio_input_close();
        return err;
    }
    ESP_LOGI(TAG, "PDM %d Hz, decimation %d, %s slot", PDM_CLOCK_HZ, PDM_DECIMATION,
             PDM_SLOT_OFFSET ? "right" : "left");
    return ESP_OK;
}

#ifdef CONFIG_VOICE_STICK_PDM_CAPTURE
static void print_base64(const uint8_t *data, size_t len)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[77];
    size_t pos = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = (uint32_t)data[i] << 16 |
                           (i + 1 < len ? (uint32_t)data[i + 1] << 8 : 0) |
                           (i + 2 < len ? data[i + 2] : 0);
        line[pos++] = alphabet[(v >> 18) & 63];
        line[pos++] = alphabet[(v >> 12) & 63];
        line[pos++] = i + 1 < len ? alphabet[(v >> 6) & 63] : '=';
        line[pos++] = i + 2 < len ? alphabet[v & 63] : '=';
        if (pos == 76 || i + 3 >= len) {
            line[pos] = '\0';
            printf("%s\n", line);
            pos = 0;
        }
    }
}

/* Bring-up: hold the prev key to record the raw PDM bitstream of the mic's
 * slot (DMA byte order, earliest bit in the MSB of each LE16 word) and dump
 * it as base64 on the console. */
static void pdm_capture_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(VS_PIN_KEY_PREV) != 0 || s_rx) {
            continue;
        }
        if (audio_input_open() != ESP_OK) {
            continue;
        }

        size_t capacity = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        capacity = capacity > 16384 ? (capacity - 8192) & ~(size_t)1 : 0;
        uint8_t *capture = capacity ? malloc(capacity) : NULL;
        size_t length = 0;
        unsigned mismatched_frames = 0;
        while (capture && length < capacity && gpio_get_level(VS_PIN_KEY_PREV) == 0) {
            if (read_chunk(READ_CHUNK_BYTES) != ESP_OK) {
                break;
            }
            for (size_t i = 0; i < READ_CHUNK_BYTES && length + 2 <= capacity; i += DMA_FRAME_BYTES) {
                capture[length++] = s_chunk[i + PDM_SLOT_OFFSET];
                capture[length++] = s_chunk[i + PDM_SLOT_OFFSET + 1];
                mismatched_frames += memcmp(&s_chunk[i], &s_chunk[i + 2], 2) != 0;
            }
        }
        audio_input_close();

        printf("PDMRAW BEGIN clock=%d bytes=%u mismatched_frames=%u\n",
               PDM_CLOCK_HZ, (unsigned)length, mismatched_frames);
        if (capture) {
            print_base64(capture, length);
        }
        printf("PDMRAW END\n");
        free(capture);

        while (gpio_get_level(VS_PIN_KEY_PREV) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

void voicestick_pdm_capture_start(void)
{
    xTaskCreate(pdm_capture_task, "pdm_capture", 4096, NULL, 3, NULL);
}
#endif

esp_err_t audio_input_read(int16_t *samples, size_t count)
{
    if (!s_rx) {
        return ESP_ERR_INVALID_STATE;
    }
    while (count > 0) {
        const size_t n = count < READ_CHUNK_SAMPLES ? count : READ_CHUNK_SAMPLES;
        esp_err_t err = read_chunk(n * PDM_WORDS_PER_SAMPLE * DMA_FRAME_BYTES);
        if (err != ESP_OK) {
            return err;
        }
        decimate_chunk(s_chunk, n, samples);
        update_stats(s_chunk, n * PDM_WORDS_PER_SAMPLE * DMA_FRAME_BYTES, samples, n);
        samples += n;
        count -= n;
    }
    return ESP_OK;
}
