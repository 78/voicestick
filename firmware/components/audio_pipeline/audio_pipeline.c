#include "audio_pipeline.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/i2s_types.h"
#include "opus.h"

#include "voice_board.h"
#include "voice_ble.h"

static const char *TAG = "audio_pipeline";

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_CHANNELS 1
#define AUDIO_FRAME_MS 60
#define AUDIO_FRAME_SAMPLES ((AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS) / 1000)
#define OPUS_BITRATE 20000
#define OPUS_MAX_PACKET_SIZE 220
#define OPUS_COMPLEXITY 1
/*
 * Mikrofonverstaerkung. 36 dB am ES8311 des StickS3 sind erprobt; 24 dB am
 * ES7210 sind der Wert aus den Beispielen von Waveshare und noch nicht am
 * Handgelenk gemessen.
 */
#if VOICE_BOARD_MIC_ES7210
#define MIC_GAIN_DB 24.0
#else
#define MIC_GAIN_DB 36.0
#endif

#define TX_QUEUE_DEPTH 50
#define TX_RETRY_DELAY_MS 30
#define TX_MAX_RETRIES 50
#define TX_DRAIN_TIMEOUT_MS 500
#define TASK_EXIT_WAIT_MS 800

typedef struct {
    uint32_t session_id;
    uint32_t seq;
    uint8_t  flags;
    uint16_t len;
    uint8_t  data[OPUS_MAX_PACKET_SIZE];
} audio_packet_t;

static atomic_bool s_running;
static atomic_uint_least8_t s_level;
static bool s_initialized;
static uint32_t s_session_id;
static uint32_t s_seq;
#define AUDIO_TASK_STACK 32768

static TaskHandle_t s_audio_task;
/*
 * Liegt der Stack des Aufnahme-Tasks im PSRAM? Dann muss der Task sich mit
 * vTaskDeleteWithCaps beenden, sonst bleiben TCB und Stack liegen.
 */
static bool s_audio_task_caps;
static TaskHandle_t s_tx_task;
static QueueHandle_t s_tx_queue;

/* Per-session resources: created on start, destroyed on stop */
static i2s_chan_handle_t s_rx_handle;
static esp_codec_dev_handle_t s_codec;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static OpusEncoder *s_opus_encoder;

static bool tasks_exited(void)
{
    return s_audio_task == NULL && s_tx_task == NULL;
}

static esp_err_t wait_for_tasks_to_exit(TickType_t timeout_ticks)
{
    TickType_t deadline = xTaskGetTickCount() + timeout_ticks;
    while (!tasks_exited()) {
        if (xTaskGetTickCount() >= deadline) {
            ESP_LOGW(TAG, "tasks still exiting: audio=%p tx=%p",
                     s_audio_task, s_tx_task);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle),
                        TAG, "create i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = VOICE_BOARD_PIN_I2S_MCLK,
            .bclk = VOICE_BOARD_PIN_I2S_BCLK,
            .ws = VOICE_BOARD_PIN_I2S_WS,
            .dout = VOICE_BOARD_PIN_I2S_DOUT,
            .din = VOICE_BOARD_PIN_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg),
                        TAG, "init i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "enable i2s rx");
    return ESP_OK;
}

static esp_err_t init_codec(void)
{
    i2c_master_bus_handle_t i2c_bus = voice_board_i2c_bus();
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "i2c bus unavailable");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_1,
#if VOICE_BOARD_MIC_ES7210
        .addr = ES7210_CODEC_DEFAULT_ADDR,
#else
        .addr = ES8311_CODEC_DEFAULT_ADDR,
#endif
        .bus_handle = i2c_bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec i2c ctrl");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_1,
        .rx_handle = s_rx_handle,
        .tx_handle = NULL,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_data_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec i2s data");

    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec gpio");

#if VOICE_BOARD_MIC_ES7210
    /*
     * Waveshare-Uhr: Die beiden Mikrofone haengen am ES7210, einem reinen
     * Analog-Digital-Wandler; der ES8311 dort treibt nur den Lautsprecher.
     * Konfiguriert wie im Board-Paket von Waveshare. Im Standard-I2S-Format
     * liegt Mikrofon 1 im linken Kanal -- genau der, den dieser Pfad mono
     * liest. Das zweite Mikrofon (Stoergeraeusch-Abzug) bleibt fuer spaeter.
     */
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = s_ctrl_if,
    };
    s_codec_if = es7210_codec_new(&es7210_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_ERR_NO_MEM, TAG, "create es7210");
#else
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    s_codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_ERR_NO_MEM, TAG, "create es8311");
#endif

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_NO_MEM, TAG, "create codec dev");

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 1,
        .channel_mask = I2S_STD_SLOT_LEFT,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open codec");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec, MIC_GAIN_DB) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set mic gain");
    return ESP_OK;
}

static esp_err_t init_opus(void)
{
    int error = OPUS_OK;
    s_opus_encoder = opus_encoder_create(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS,
                                         OPUS_APPLICATION_VOIP, &error);
    ESP_RETURN_ON_FALSE(s_opus_encoder != NULL && error == OPUS_OK,
                        ESP_FAIL, TAG, "create opus encoder error=%d", error);
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_VBR(0));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_DTX(0));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    return ESP_OK;
}

static void deinit_opus(void)
{
    if (s_opus_encoder) {
        opus_encoder_destroy(s_opus_encoder);
        s_opus_encoder = NULL;
    }
}

static void deinit_codec(void)
{
    if (s_codec) {
        esp_codec_dev_close(s_codec);
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
    }
    if (s_codec_if) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_data_if) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_gpio_if) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_ctrl_if) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
}

static void deinit_i2s(void)
{
    if (s_rx_handle) {
        /* Channel is already disabled by codec close; just release it. */
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }
}

static void deinit_session_resources(void)
{
    deinit_opus();
    deinit_codec();
    deinit_i2s();
    ESP_LOGI(TAG, "session resources released");
}

static void audio_task(void *arg)
{
    (void)arg;
    int16_t mono[AUDIO_FRAME_SAMPLES];
    uint8_t opus_buf[OPUS_MAX_PACKET_SIZE];
    uint32_t enqueued = 0;
    uint32_t dropped = 0;
    uint8_t level = 0;

    while (atomic_load(&s_running)) {
        esp_err_t err = esp_codec_dev_read(s_codec, mono, sizeof(mono));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "codec read failed: %s", esp_err_to_name(err));
            continue;
        }

        /*
         * Spitzenwert je Rahmen, wurzelfoermig gestaucht und mit Nachlauf:
         * ohne den Nachlauf zittert der Balken im Sprechrhythmus so stark,
         * dass man nicht mehr erkennt, ob ueberhaupt etwas ankommt.
         */
        int32_t peak = 0;
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; ++i) {
            const int32_t value = mono[i] < 0 ? -(int32_t)mono[i] : (int32_t)mono[i];
            if (value > peak) {
                peak = value;
            }
        }
        const uint8_t fresh = (uint8_t)(100.0f * sqrtf((float)peak / 32768.0f));
        level = fresh > level ? fresh : (uint8_t)((level * 3) / 4);
        atomic_store(&s_level, level);

        opus_int32 encoded = opus_encode(s_opus_encoder, mono, AUDIO_FRAME_SAMPLES,
                                         opus_buf, sizeof(opus_buf));
        if (encoded < 0) {
            ESP_LOGE(TAG, "opus encode failed: %d", (int)encoded);
            continue;
        }

        audio_packet_t pkt = {
            .session_id = s_session_id,
            .seq = s_seq,
            .flags = (s_seq == 0) ? VOICE_BLE_FLAG_START : 0x00,
            .len = (uint16_t)encoded,
        };
        memcpy(pkt.data, opus_buf, encoded);

        if (xQueueSend(s_tx_queue, &pkt, 0) == pdTRUE) {
            s_seq++;
            enqueued++;
        } else {
            /* Queue full: drop oldest packet to make room */
            audio_packet_t discard;
            xQueueReceive(s_tx_queue, &discard, 0);
            xQueueSend(s_tx_queue, &pkt, 0);
            s_seq++;
            enqueued++;
            dropped++;
            if (dropped == 1 || (dropped % 20) == 0) {
                ESP_LOGW(TAG, "tx queue overflow, dropped oldest (total=%" PRIu32 ")", dropped);
            }
        }
    }

    atomic_store(&s_level, 0);
    ESP_LOGI(TAG, "audio task exit: enqueued=%" PRIu32 " overflow_drops=%" PRIu32,
             enqueued, dropped);
    s_audio_task = NULL;
    if (s_audio_task_caps) {
        vTaskDeleteWithCaps(NULL);
    } else {
        vTaskDelete(NULL);
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    audio_packet_t pkt;
    uint32_t sent = 0;
    uint32_t tx_dropped = 0;

    while (true) {
        if (xQueueReceive(s_tx_queue, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Sentinel: END flag with no payload signals drain mode */
        if (pkt.flags == VOICE_BLE_FLAG_END && pkt.len == 0) {
            goto drain;
        }

        int retries = 0;
        while (true) {
            esp_err_t err = voice_ble_send_audio(pkt.session_id, pkt.seq,
                                                 pkt.flags, pkt.data, pkt.len);
            if (err == ESP_OK) {
                sent++;
                break;
            }
            retries++;
            if (retries >= TX_MAX_RETRIES) {
                tx_dropped++;
                if (tx_dropped == 1 || (tx_dropped % 20) == 0) {
                    ESP_LOGW(TAG, "tx failed after %d retries seq=%" PRIu32
                             " (total_dropped=%" PRIu32 ")",
                             TX_MAX_RETRIES, pkt.seq, tx_dropped);
                }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(TX_RETRY_DELAY_MS));
        }
    }

drain:
    {
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TX_DRAIN_TIMEOUT_MS);
        while (xQueueReceive(s_tx_queue, &pkt, 0) == pdTRUE) {
            if (pkt.flags == VOICE_BLE_FLAG_END && pkt.len == 0) {
                break;
            }
            if (xTaskGetTickCount() >= deadline) {
                UBaseType_t remaining = uxQueueMessagesWaiting(s_tx_queue);
                if (remaining > 0) {
                    ESP_LOGW(TAG, "drain timeout, discarding %u packets",
                             (unsigned)remaining);
                }
                xQueueReset(s_tx_queue);
                break;
            }
            int retries = 0;
            while (true) {
                esp_err_t err = voice_ble_send_audio(pkt.session_id, pkt.seq,
                                                     pkt.flags, pkt.data, pkt.len);
                if (err == ESP_OK) {
                    sent++;
                    break;
                }
                retries++;
                if (retries >= TX_MAX_RETRIES || xTaskGetTickCount() >= deadline) {
                    tx_dropped++;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(TX_RETRY_DELAY_MS));
            }
        }

        /* Send the END marker over BLE */
        voice_ble_send_audio(s_session_id, s_seq, VOICE_BLE_FLAG_END, NULL, 0);

        ESP_LOGI(TAG, "tx task exit: sent=%" PRIu32 " dropped=%" PRIu32, sent, tx_dropped);

        /* Wait for audio_task to finish before destroying shared resources */
        while (s_audio_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        deinit_session_resources();
        s_tx_task = NULL;
        vTaskDelete(NULL);
    }
}

esp_err_t audio_pipeline_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(audio_packet_t));
    ESP_RETURN_ON_FALSE(s_tx_queue != NULL, ESP_ERR_NO_MEM, TAG, "create tx queue");

    s_initialized = true;
    ESP_LOGI(TAG, "audio pipeline ready (resources allocated on demand)");
    return ESP_OK;
}

esp_err_t audio_pipeline_start(uint32_t session_id)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (atomic_load(&s_running)) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(wait_for_tasks_to_exit(pdMS_TO_TICKS(TASK_EXIT_WAIT_MS)),
                        TAG, "wait previous session exit");

    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "i2s init");
    esp_err_t err = init_codec();
    if (err != ESP_OK) {
        deinit_i2s();
        ESP_LOGE(TAG, "codec init: %s", esp_err_to_name(err));
        return err;
    }
    err = init_opus();
    if (err != ESP_OK) {
        deinit_codec();
        deinit_i2s();
        ESP_LOGE(TAG, "opus init: %s", esp_err_to_name(err));
        return err;
    }

    voice_ble_request_fast_interval();

    xQueueReset(s_tx_queue);
    s_session_id = session_id;
    s_seq = 0;
    opus_encoder_ctl(s_opus_encoder, OPUS_RESET_STATE);
    atomic_store(&s_running, true);

    BaseType_t ok = xTaskCreatePinnedToCore(tx_task, "audio_tx", 4096,
                                            NULL, 6, &s_tx_task, 0);
    if (ok != pdPASS) {
        atomic_store(&s_running, false);
        s_tx_task = NULL;
        deinit_session_resources();
        return ESP_ERR_NO_MEM;
    }

    /*
     * 32 KB Stack fuer Opus — und zwar im PSRAM, nicht im internen RAM.
     *
     * Bis 0.5.1 kam der Stack aus dem internen Heap, als ein zusammenhaengender
     * Block bei jedem Tastendruck. Mit jedem Paket wuchs der feste Verbrauch
     * (Listen, Verlauf, groessere Stacks fuer Bluetooth und Ereignis-Task), bis
     * 0.5.1 keinen 32-KB-Block mehr fand: "Aufnahme startet nicht:
     * ESP_ERR_NO_MEM". Der Task liest nur I2S und rechnet Opus, schreibt nie
     * ins Flash — damit darf sein Stack im PSRAM liegen. Gelingt das nicht,
     * bleibt der alte Weg als Rueckfall.
     */
    s_audio_task_caps = true;
    ok = xTaskCreatePinnedToCoreWithCaps(audio_task, "audio_pipeline", AUDIO_TASK_STACK,
                                         NULL, 5, &s_audio_task, 1,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "audio stack in PSRAM failed, trying internal RAM");
        s_audio_task_caps = false;
        ok = xTaskCreatePinnedToCore(audio_task, "audio_pipeline", AUDIO_TASK_STACK,
                                     NULL, 5, &s_audio_task, 1);
    }
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create audio task: internal free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        atomic_store(&s_running, false);
        s_audio_task = NULL;
        audio_packet_t sentinel = {
            .session_id = s_session_id,
            .seq = s_seq,
            .flags = VOICE_BLE_FLAG_END,
            .len = 0,
        };
        xQueueSend(s_tx_queue, &sentinel, portMAX_DELAY);
        (void)wait_for_tasks_to_exit(pdMS_TO_TICKS(TASK_EXIT_WAIT_MS));
        /* tx_task cleans up session resources on exit */
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "start session %" PRIu32, session_id);
    return ESP_OK;
}

esp_err_t audio_pipeline_stop(void)
{
    if (!atomic_load(&s_running)) {
        return ESP_OK;
    }
    atomic_store(&s_running, false);
    ESP_LOGI(TAG, "stop session %" PRIu32, s_session_id);

    /* Send sentinel to trigger tx_task drain and exit */
    audio_packet_t sentinel = {
        .session_id = s_session_id,
        .seq = s_seq,
        .flags = VOICE_BLE_FLAG_END,
        .len = 0,
    };
    xQueueSend(s_tx_queue, &sentinel, portMAX_DELAY);
    return ESP_OK;
}

uint32_t audio_pipeline_session_id(void)
{
    return s_session_id;
}

uint8_t audio_pipeline_level(void)
{
    return (uint8_t)atomic_load(&s_level);
}

bool audio_pipeline_is_busy(void)
{
    return atomic_load(&s_running) || !tasks_exited();
}
