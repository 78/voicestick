#include "audio_pipeline.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "opus.h"

#include "audio_input.h"
#include "voice_ble.h"

static const char *TAG = "audio_pipeline";

#define AUDIO_SAMPLE_RATE AUDIO_INPUT_SAMPLE_RATE
#define AUDIO_CHANNELS 1
#define AUDIO_FRAME_MS 60
#define AUDIO_FRAME_SAMPLES ((AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS) / 1000)
#define OPUS_BITRATE 20000
#define OPUS_MAX_PACKET_SIZE 220
#define OPUS_COMPLEXITY CONFIG_VOICE_STICK_OPUS_COMPLEXITY

#define TX_QUEUE_DEPTH 50
#define TX_RETRY_DELAY_MS 30
#define TX_MAX_RETRIES 50
#define TX_DRAIN_TIMEOUT_MS 500
#define TASK_EXIT_WAIT_MS 800

#define AUDIO_TASK_STACK 32768
/* Encode on the second core when there is one; BLE runs on core 0. */
#define AUDIO_TASK_CORE (portNUM_PROCESSORS - 1)

typedef struct {
    uint16_t stream_id;
    uint32_t seq;
    uint32_t timestamp_ms;
    uint8_t  flags;
    uint16_t len;
    uint8_t  data[OPUS_MAX_PACKET_SIZE];
} audio_packet_t;

static atomic_bool s_running;
static bool s_initialized;
static uint16_t s_stream_id;
static atomic_uint_least32_t s_seq;
static TaskHandle_t s_audio_task;
static TaskHandle_t s_tx_task;
static QueueHandle_t s_tx_queue;

/* Per-capture resources: created on start, destroyed on stop. */
static OpusEncoder *s_opus_encoder;

static uint32_t now_timestamp_ms(void)
{
    return (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
}

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

static void deinit_session_resources(void)
{
    deinit_opus();
    audio_input_close();
    ESP_LOGI(TAG, "audio capture resources released");
}

static void enqueue_packet(const audio_packet_t *pkt, uint32_t *dropped)
{
    if (xQueueSend(s_tx_queue, pkt, 0) == pdTRUE) {
        return;
    }
    /* Queue full: drop oldest packet to make room. */
    audio_packet_t discard;
    (void)xQueueReceive(s_tx_queue, &discard, 0);
    (void)xQueueSend(s_tx_queue, pkt, 0);
    if (dropped) {
        (*dropped)++;
        if (*dropped == 1 || (*dropped % 20) == 0) {
            ESP_LOGW(TAG, "tx queue overflow, dropped oldest (total=%" PRIu32 ")", *dropped);
        }
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    int16_t mono[AUDIO_FRAME_SAMPLES];
    uint8_t opus_buf[OPUS_MAX_PACKET_SIZE];
    uint32_t enqueued = 0;
    uint32_t dropped = 0;

    while (atomic_load(&s_running)) {
        esp_err_t err = audio_input_read(mono, AUDIO_FRAME_SAMPLES);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "audio read failed: %s", esp_err_to_name(err));
            continue;
        }
        if (!atomic_load(&s_running)) {
            break;
        }

        const uint32_t ts_ms = now_timestamp_ms();
        opus_int32 encoded = opus_encode(s_opus_encoder, mono, AUDIO_FRAME_SAMPLES,
                                         opus_buf, sizeof(opus_buf));
        if (encoded < 0) {
            ESP_LOGE(TAG, "opus encode failed: %d", (int)encoded);
            continue;
        }

        audio_packet_t pkt = {
            .stream_id = s_stream_id,
            .seq = atomic_fetch_add(&s_seq, 1),
            .timestamp_ms = ts_ms,
            .flags = 0x00,
            .len = (uint16_t)encoded,
        };
        memcpy(pkt.data, opus_buf, encoded);
        enqueue_packet(&pkt, &dropped);
        enqueued++;
    }

    audio_packet_t end_pkt = {
        .stream_id = s_stream_id,
        .seq = atomic_fetch_add(&s_seq, 1),
        .timestamp_ms = now_timestamp_ms(),
        .flags = VOICE_BLE_FLAG_END,
        .len = 0,
    };
    enqueue_packet(&end_pkt, NULL);

    ESP_LOGI(TAG, "audio task exit: enqueued=%" PRIu32 " overflow_drops=%" PRIu32,
             enqueued, dropped);
    s_audio_task = NULL;
    vTaskDelete(NULL);
}

static bool send_packet_with_retry(const audio_packet_t *pkt, TickType_t deadline, bool has_deadline)
{
    int retries = 0;
    while (true) {
        esp_err_t err = voice_ble_send_audio(pkt->stream_id, pkt->seq, pkt->timestamp_ms,
                                             pkt->flags, pkt->len ? pkt->data : NULL, pkt->len);
        if (err == ESP_OK) {
            return true;
        }
        if (err == ESP_ERR_INVALID_STATE) {
            return false;
        }
        retries++;
        if (retries >= TX_MAX_RETRIES || (has_deadline && xTaskGetTickCount() >= deadline)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(TX_RETRY_DELAY_MS));
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    audio_packet_t pkt;
    uint32_t sent = 0;
    uint32_t tx_dropped = 0;
    bool draining = false;
    TickType_t deadline = 0;

    while (true) {
        if (xQueueReceive(s_tx_queue, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!draining && !atomic_load(&s_running)) {
            draining = true;
            deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TX_DRAIN_TIMEOUT_MS);
        }

        if (pkt.flags == VOICE_BLE_FLAG_END && pkt.len == 0) {
            (void)send_packet_with_retry(&pkt, xTaskGetTickCount() + pdMS_TO_TICKS(200), true);
            break;
        }

        if (draining && xTaskGetTickCount() >= deadline) {
            tx_dropped++;
            continue;
        }

        if (send_packet_with_retry(&pkt, deadline, draining)) {
            sent++;
        } else {
            tx_dropped++;
            if (tx_dropped == 1 || (tx_dropped % 20) == 0) {
                ESP_LOGW(TAG, "tx failed seq=%" PRIu32 " (total_dropped=%" PRIu32 ")",
                         pkt.seq, tx_dropped);
            }
        }
    }

    ESP_LOGI(TAG, "tx task exit: sent=%" PRIu32 " dropped=%" PRIu32, sent, tx_dropped);

    while (s_audio_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    deinit_session_resources();
    s_tx_task = NULL;
    vTaskDelete(NULL);
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

esp_err_t audio_pipeline_start(uint16_t stream_id, uint32_t initial_seq)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (atomic_load(&s_running)) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(wait_for_tasks_to_exit(pdMS_TO_TICKS(TASK_EXIT_WAIT_MS)),
                        TAG, "wait previous capture exit");

    ESP_RETURN_ON_ERROR(audio_input_open(), TAG, "audio input open");
    esp_err_t err = init_opus();
    if (err != ESP_OK) {
        audio_input_close();
        ESP_LOGE(TAG, "opus init: %s", esp_err_to_name(err));
        return err;
    }

    xQueueReset(s_tx_queue);
    s_stream_id = stream_id;
    atomic_store(&s_seq, initial_seq);
    opus_encoder_ctl(s_opus_encoder, OPUS_RESET_STATE);
    atomic_store(&s_running, true);

    /* Immediately enqueue the zero-payload START marker before the first Opus frame finishes. */
    audio_packet_t start_pkt = {
        .stream_id = s_stream_id,
        .seq = atomic_fetch_add(&s_seq, 1),
        .timestamp_ms = now_timestamp_ms(),
        .flags = VOICE_BLE_FLAG_START,
        .len = 0,
    };
    (void)xQueueSend(s_tx_queue, &start_pkt, 0);

    BaseType_t ok = xTaskCreatePinnedToCore(tx_task, "audio_tx", 4096,
                                            NULL, 6, &s_tx_task, 0);
    if (ok != pdPASS) {
        atomic_store(&s_running, false);
        s_tx_task = NULL;
        deinit_session_resources();
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreatePinnedToCore(audio_task, "audio_pipeline", AUDIO_TASK_STACK,
                                 NULL, 5, &s_audio_task, AUDIO_TASK_CORE);
    if (ok != pdPASS) {
        atomic_store(&s_running, false);
        s_audio_task = NULL;
        audio_packet_t end_pkt = {
            .stream_id = s_stream_id,
            .seq = atomic_fetch_add(&s_seq, 1),
            .timestamp_ms = now_timestamp_ms(),
            .flags = VOICE_BLE_FLAG_END,
            .len = 0,
        };
        (void)xQueueSend(s_tx_queue, &end_pkt, portMAX_DELAY);
        (void)wait_for_tasks_to_exit(pdMS_TO_TICKS(TASK_EXIT_WAIT_MS));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "start stream=%u seq=%" PRIu32, stream_id, initial_seq);
    return ESP_OK;
}

esp_err_t audio_pipeline_stop(bool wait_for_exit)
{
    if (atomic_load(&s_running)) {
        atomic_store(&s_running, false);
        ESP_LOGI(TAG, "stop stream=%u", s_stream_id);
    }
    if (wait_for_exit) {
        return wait_for_tasks_to_exit(pdMS_TO_TICKS(TASK_EXIT_WAIT_MS));
    }
    return ESP_OK;
}

bool audio_pipeline_is_running(void)
{
    return atomic_load(&s_running);
}

uint16_t audio_pipeline_stream_id(void)
{
    return s_stream_id;
}

uint32_t audio_pipeline_next_seq(void)
{
    return atomic_load(&s_seq);
}
