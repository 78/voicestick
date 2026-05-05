#include "voice_ble.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "voice_ble";

#define OTA_PROGRESS_NOTIFY_BYTES (32 * 1024)

static bool s_connected;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static uint16_t s_audio_attr_handle;
static uint16_t s_state_attr_handle;
static uint16_t s_ota_state_attr_handle;
static char s_device_id[5] = "0000";
static char s_device_name[8] = VOICE_BLE_DEVICE_NAME_PREFIX "-0000";
static voice_ble_connection_cb_t s_connection_cb;
static voice_ble_ota_cb_t s_ota_cb;

typedef struct {
    bool active;
    uint32_t transfer_id;
    uint32_t image_size;
    uint32_t written;
    uint32_t next_progress;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
} voice_ble_ota_state_t;

static voice_ble_ota_state_t s_ota;

static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0x00, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_audio_uuid =
    BLE_UUID128_INIT(0x01, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_state_uuid =
    BLE_UUID128_INIT(0x02, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_control_uuid =
    BLE_UUID128_INIT(0x03, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_ota_rx_uuid =
    BLE_UUID128_INIT(0x04, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_ota_state_uuid =
    BLE_UUID128_INIT(0x05, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);

static void start_advertising(void);
static void stop_advertising(void);

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void ota_clear_state(void)
{
    memset(&s_ota, 0, sizeof(s_ota));
}

static esp_err_t ota_send_state_json(const char *json)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE ||
        s_ota_state_attr_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t json_len = strlen(json);
    uint8_t header[4] = {
        1,
        VOICE_BLE_OTA_TYPE_STATE,
        json_len & 0xff,
        json_len >> 8,
    };

    struct os_mbuf *om = ble_hs_mbuf_from_flat(header, sizeof(header));
    if (!om) {
        return ESP_ERR_NO_MEM;
    }

    int rc = os_mbuf_append(om, json, json_len);
    if (rc != 0) {
        os_mbuf_free_chain(om);
        return ESP_FAIL;
    }

    rc = ble_gatts_notify_custom(s_conn_handle, s_ota_state_attr_handle, om);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

static void ota_send_error(const char *code, esp_err_t err)
{
    char json[128];
    snprintf(json, sizeof(json),
             "{\"event\":\"error\",\"code\":\"%s\",\"esp_err\":%d}",
             code, (int)err);
    (void)ota_send_state_json(json);
    if (s_ota_cb) {
        s_ota_cb(VOICE_BLE_OTA_EVENT_ERROR, s_ota.written, s_ota.image_size);
    }
}

static int ota_begin(uint32_t transfer_id, uint32_t image_size)
{
    if (s_ota.active) {
        (void)esp_ota_abort(s_ota.handle);
        ota_clear_state();
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        ota_send_error("no_partition", ESP_ERR_NOT_FOUND);
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (image_size == 0 || image_size > partition->size) {
        ota_send_error("bad_size", ESP_ERR_INVALID_SIZE);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(partition, image_size, &handle);
    if (err != ESP_OK) {
        ota_send_error("begin_failed", err);
        return BLE_ATT_ERR_UNLIKELY;
    }

    s_ota.active = true;
    s_ota.transfer_id = transfer_id;
    s_ota.image_size = image_size;
    s_ota.written = 0;
    s_ota.next_progress = OTA_PROGRESS_NOTIFY_BYTES;
    s_ota.handle = handle;
    s_ota.partition = partition;

    char json[160];
    snprintf(json, sizeof(json),
             "{\"event\":\"ready\",\"transfer_id\":%" PRIu32
             ",\"size\":%" PRIu32 ",\"partition\":\"%s\"}",
             transfer_id, image_size, partition->label);
    (void)ota_send_state_json(json);
    if (s_ota_cb) {
        s_ota_cb(VOICE_BLE_OTA_EVENT_BEGIN, 0, image_size);
    }
    ESP_LOGI(TAG, "OTA begin transfer=%" PRIu32 " size=%" PRIu32 " partition=%s",
             transfer_id, image_size, partition->label);
    return 0;
}

static int ota_write_data(uint32_t transfer_id, uint32_t offset,
                          const uint8_t *payload, uint16_t payload_len)
{
    if (!s_ota.active || transfer_id != s_ota.transfer_id) {
        ota_send_error("not_active", ESP_ERR_INVALID_STATE);
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (offset != s_ota.written ||
        payload_len == 0 ||
        s_ota.written + payload_len > s_ota.image_size) {
        ota_send_error("bad_offset", ESP_ERR_INVALID_ARG);
        return BLE_ATT_ERR_INVALID_OFFSET;
    }

    esp_err_t err = esp_ota_write(s_ota.handle, payload, payload_len);
    if (err != ESP_OK) {
        ota_send_error("write_failed", err);
        return BLE_ATT_ERR_UNLIKELY;
    }

    s_ota.written += payload_len;
    if (s_ota.written >= s_ota.next_progress || s_ota.written == s_ota.image_size) {
        char json[128];
        snprintf(json, sizeof(json),
                 "{\"event\":\"progress\",\"transfer_id\":%" PRIu32
                 ",\"written\":%" PRIu32 ",\"size\":%" PRIu32 "}",
                 s_ota.transfer_id, s_ota.written, s_ota.image_size);
        (void)ota_send_state_json(json);
        if (s_ota_cb) {
            s_ota_cb(VOICE_BLE_OTA_EVENT_PROGRESS, s_ota.written, s_ota.image_size);
        }
        while (s_ota.written >= s_ota.next_progress) {
            s_ota.next_progress += OTA_PROGRESS_NOTIFY_BYTES;
        }
    }

    return 0;
}

static int ota_finish(uint32_t transfer_id, uint32_t image_size)
{
    if (!s_ota.active || transfer_id != s_ota.transfer_id) {
        ota_send_error("not_active", ESP_ERR_INVALID_STATE);
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (image_size != s_ota.image_size || s_ota.written != s_ota.image_size) {
        ota_send_error("incomplete", ESP_ERR_INVALID_SIZE);
        return BLE_ATT_ERR_UNLIKELY;
    }

    esp_err_t err = esp_ota_end(s_ota.handle);
    if (err != ESP_OK) {
        ota_clear_state();
        ota_send_error("end_failed", err);
        return BLE_ATT_ERR_UNLIKELY;
    }

    err = esp_ota_set_boot_partition(s_ota.partition);
    if (err != ESP_OK) {
        ota_clear_state();
        ota_send_error("set_boot_failed", err);
        return BLE_ATT_ERR_UNLIKELY;
    }

    char json[128];
    snprintf(json, sizeof(json),
             "{\"event\":\"done\",\"transfer_id\":%" PRIu32 ",\"reboot_ms\":500}",
             transfer_id);
    (void)ota_send_state_json(json);
    if (s_ota_cb) {
        s_ota_cb(VOICE_BLE_OTA_EVENT_DONE, s_ota.written, s_ota.image_size);
    }
    ESP_LOGI(TAG, "OTA complete transfer=%" PRIu32 ", rebooting", transfer_id);
    ota_clear_state();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return 0;
}

static int ota_abort(uint32_t transfer_id)
{
    if (s_ota.active && transfer_id == s_ota.transfer_id) {
        (void)esp_ota_abort(s_ota.handle);
        ESP_LOGI(TAG, "OTA aborted transfer=%" PRIu32, transfer_id);
    }
    ota_clear_state();
    (void)ota_send_state_json("{\"event\":\"aborted\"}");
    if (s_ota_cb) {
        s_ota_cb(VOICE_BLE_OTA_EVENT_ABORT, 0, 0);
    }
    return 0;
}

static int ota_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 4) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t *buffer = malloc(len);
    if (!buffer) {
        ota_send_error("no_mem", ESP_ERR_NO_MEM);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    int rc = ble_hs_mbuf_to_flat(ctxt->om, buffer, len, NULL);
    if (rc != 0) {
        free(buffer);
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint8_t version = buffer[0];
    const uint8_t type = buffer[1];
    const uint16_t header_len = read_le16(&buffer[2]);
    if (version != 1 || header_len > len) {
        free(buffer);
        return BLE_ATT_ERR_UNLIKELY;
    }

    switch (type) {
    case VOICE_BLE_OTA_TYPE_BEGIN:
        if (header_len != 12) {
            rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            break;
        }
        rc = ota_begin(read_le32(&buffer[8]), read_le32(&buffer[4]));
        break;
    case VOICE_BLE_OTA_TYPE_DATA:
        if (header_len != 12) {
            rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            break;
        }
        rc = ota_write_data(read_le32(&buffer[4]), read_le32(&buffer[8]),
                            &buffer[header_len], len - header_len);
        break;
    case VOICE_BLE_OTA_TYPE_END:
        if (header_len != 12) {
            rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            break;
        }
        rc = ota_finish(read_le32(&buffer[4]), read_le32(&buffer[8]));
        break;
    case VOICE_BLE_OTA_TYPE_ABORT:
        if (header_len != 8) {
            rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            break;
        }
        rc = ota_abort(read_le32(&buffer[4]));
        break;
    default:
        rc = BLE_ATT_ERR_UNLIKELY;
        break;
    }

    free(buffer);
    return rc;
}

static int control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    char buffer[128] = {0};
    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    const uint16_t copy_len = MIN(len, sizeof(buffer) - 1);
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buffer, copy_len, NULL);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    ESP_LOGD(TAG, "control %s", buffer);
    return 0;
}

static int notify_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_audio_uuid.u,
                .access_cb = notify_access_cb,
                .val_handle = &s_audio_attr_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &s_state_uuid.u,
                .access_cb = notify_access_cb,
                .val_handle = &s_state_attr_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &s_control_uuid.u,
                .access_cb = control_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_ota_rx_uuid.u,
                .access_cb = ota_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_ota_state_uuid.u,
                .access_cb = notify_access_cb,
                .val_handle = &s_ota_state_attr_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connected = true;
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected handle=%u", s_conn_handle);
            stop_advertising();
            if (s_connection_cb) {
                s_connection_cb(true);
            }
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            start_advertising();
            if (s_connection_cb) {
                s_connection_cb(false);
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
        if (s_ota.active) {
            (void)esp_ota_abort(s_ota.handle);
            ota_clear_state();
            if (s_ota_cb) {
                s_ota_cb(VOICE_BLE_OTA_EVENT_ABORT, 0, 0);
            }
            ESP_LOGI(TAG, "OTA aborted after disconnect");
        }
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        start_advertising();
        if (s_connection_cb) {
            s_connection_cb(false);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGD(TAG, "subscribe attr=%u notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGD(TAG, "mtu=%u", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void start_advertising(void)
{
    if (s_connected) {
        ESP_LOGD(TAG, "skip advertising while connected");
        return;
    }

    if (ble_gap_adv_active()) {
        ESP_LOGD(TAG, "advertising already active");
        return;
    }

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set adv fields failed rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (const uint8_t *)s_device_name;
    rsp_fields.name_len = strlen(s_device_name);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set scan response failed rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(60);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(120);

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "start advertising failed rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "advertising as %s", s_device_name);
}

static void stop_advertising(void)
{
    if (!ble_gap_adv_active()) {
        return;
    }

    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "stop advertising failed rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "advertising stopped");
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer own addr type failed rc=%d", rc);
        return;
    }

    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "reset reason=%d", reason);
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t init_device_identity(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    ESP_RETURN_ON_ERROR(err, TAG, "read base mac failed");

    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X", mac[4], mac[5]);
    snprintf(s_device_name, sizeof(s_device_name), "%s-%s",
             VOICE_BLE_DEVICE_NAME_PREFIX, s_device_id);
    return ESP_OK;
}

esp_err_t voice_ble_init(void)
{
    ESP_RETURN_ON_ERROR(init_device_identity(), TAG, "device identity init failed");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init failed");

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init failed");

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    int rc = ble_svc_gap_device_name_set(s_device_name);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "set device name failed rc=%d", rc);

    rc = ble_gatts_count_cfg(s_gatt_services);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "count gatt failed rc=%d", rc);
    rc = ble_gatts_add_svcs(s_gatt_services);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "add gatt failed rc=%d", rc);

    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "BLE initialized as %s", s_device_name);
    return ESP_OK;
}

const char *voice_ble_device_id(void)
{
    return s_device_id;
}

const char *voice_ble_device_name(void)
{
    return s_device_name;
}

void voice_ble_set_connection_callback(voice_ble_connection_cb_t callback)
{
    s_connection_cb = callback;
}

void voice_ble_set_ota_callback(voice_ble_ota_cb_t callback)
{
    s_ota_cb = callback;
}

bool voice_ble_is_connected(void)
{
    return s_connected;
}

bool voice_ble_ota_is_active(void)
{
    return s_ota.active;
}

esp_err_t voice_ble_send_audio(uint32_t session_id, uint32_t seq, uint8_t flags,
                               const uint8_t *opus_payload, size_t len)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > UINT16_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGD(TAG, "audio session=%" PRIu32 " seq=%" PRIu32 " flags=0x%02x len=%u",
             session_id, seq, flags, (unsigned)len);

    uint8_t header[16] = {
        1,
        0x01,
        16,
        0,
        session_id & 0xff,
        (session_id >> 8) & 0xff,
        (session_id >> 16) & 0xff,
        (session_id >> 24) & 0xff,
        seq & 0xff,
        (seq >> 8) & 0xff,
        (seq >> 16) & 0xff,
        (seq >> 24) & 0xff,
        flags,
        0,
        len & 0xff,
        (len >> 8) & 0xff,
    };

    struct os_mbuf *om = ble_hs_mbuf_from_flat(header, sizeof(header));
    if (!om) {
        return ESP_ERR_NO_MEM;
    }

    if (len > 0) {
        int rc = os_mbuf_append(om, opus_payload, len);
        if (rc != 0) {
            os_mbuf_free_chain(om);
            return ESP_FAIL;
        }
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_audio_attr_handle, om);
    if (rc != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t send_state_json(const char *json)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t json_len = strlen(json);
    uint8_t header[4] = {
        1,
        0x10,
        json_len & 0xff,
        json_len >> 8,
    };

    struct os_mbuf *om = ble_hs_mbuf_from_flat(header, sizeof(header));
    if (!om) {
        return ESP_ERR_NO_MEM;
    }

    int rc = os_mbuf_append(om, json, json_len);
    if (rc != 0) {
        os_mbuf_free_chain(om);
        return ESP_FAIL;
    }

    rc = ble_gatts_notify_custom(s_conn_handle, s_state_attr_handle, om);
    if (rc != 0) {
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "state %s", json);
    return ESP_OK;
}

esp_err_t voice_ble_send_press_start(uint32_t session_id)
{
    char json[80];
    snprintf(json, sizeof(json), "{\"event\":\"press_start\",\"session_id\":%" PRIu32 "}", session_id);
    return send_state_json(json);
}

esp_err_t voice_ble_send_press_end(uint32_t session_id)
{
    char json[80];
    snprintf(json, sizeof(json), "{\"event\":\"press_end\",\"session_id\":%" PRIu32 "}", session_id);
    return send_state_json(json);
}

esp_err_t voice_ble_send_cancel(void)
{
    return send_state_json("{\"event\":\"cancel\"}");
}
