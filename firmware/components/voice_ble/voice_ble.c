#include "voice_ble.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "nimble/hci_common.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "board.h"

static const char *TAG = "voice_ble";

#define OTA_PROGRESS_NOTIFY_BYTES (32 * 1024)
#define PAIRING_WINDOW_MS (60 * 1000)
#define LINK_SECURITY_GRACE_MS 1500
#define ADV_RETRY_DELAY_MS 1000
#define CONTROL_REASM_TIMEOUT_US (2 * 1000000LL)
#define IMU_MIN_FREE_MBUFS 16

static atomic_bool s_connected;
static atomic_bool s_stream_tx_subscribed;
static atomic_bool s_control_tx_subscribed;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static atomic_uint_least16_t s_link_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static uint16_t s_stream_tx_attr_handle;
static uint16_t s_control_tx_attr_handle;
static uint16_t s_ota_state_attr_handle;
static char s_device_id[5] = "0000";
static char s_device_name[8] = VOICE_BLE_DEVICE_NAME_PREFIX "-0000";
static char s_device_uuid[37];
static voice_ble_connection_cb_t s_connection_cb;
static voice_ble_pairing_cb_t s_pairing_cb;
static voice_ble_control_cb_t s_control_cb;
static voice_ble_stream_rx_cb_t s_stream_rx_cb;
static voice_ble_ota_cb_t s_ota_cb;
static atomic_bool s_ble_initialized;
static atomic_uint_least32_t s_last_tx_ms;

static void record_tx_activity(void)
{
    atomic_store(&s_last_tx_ms, (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL));
}

/* Control TX serialization and sequence tracking. */
static SemaphoreHandle_t s_control_tx_lock;
static uint8_t s_tx_msg_id;
static uint32_t s_event_seq;

/* Control RX chunk reassembly (owned by NimBLE host task). */
static struct {
    bool active;
    uint8_t msg_id;
    uint8_t next_index;
    uint8_t count;
    size_t len;
    int64_t started_us;
    char buf[VOICE_BLE_MAX_CONTROL_MESSAGE_BYTES + 1];
} s_rx_reasm;

/* The NimBLE host task owns pairing changes and bond storage. */
static atomic_bool s_pairing_open;
static bool s_has_owner;
static ble_addr_t s_owner;
static nvs_handle_t s_owner_store;
static const char *OWNER_KEY = "owner";
static struct ble_npl_event s_open_pairing_event;
static struct ble_npl_callout s_pairing_timeout;
static struct ble_npl_callout s_link_security;
static struct ble_npl_callout s_adv_retry_callout;

typedef enum {
    CONN_ITVL_NONE,
    CONN_ITVL_FAST,
    CONN_ITVL_SLOW,
} conn_itvl_target_t;

static conn_itvl_target_t s_itvl_target;
static bool s_itvl_update_pending;
static bool s_itvl_update_in_flight;

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
static const ble_uuid128_t s_stream_tx_uuid =
    BLE_UUID128_INIT(0x01, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_control_tx_uuid =
    BLE_UUID128_INIT(0x02, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_control_rx_uuid =
    BLE_UUID128_INIT(0x03, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_ota_rx_uuid =
    BLE_UUID128_INIT(0x04, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_ota_state_uuid =
    BLE_UUID128_INIT(0x05, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_pairing_adv_uuid =
    BLE_UUID128_INIT(0x06, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_reconnect_adv_uuid =
    BLE_UUID128_INIT(0x07, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_authorized_uuid =
    BLE_UUID128_INIT(0x08, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_stream_rx_uuid =
    BLE_UUID128_INIT(0x09, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);

void ble_store_config_init(void);

static void start_advertising(void);
static void stop_advertising(void);

static void reset_rx_reasm(void)
{
    s_rx_reasm.active = false;
    s_rx_reasm.len = 0;
    s_rx_reasm.next_index = 0;
    s_rx_reasm.count = 0;
}

static void adv_retry_callout_cb(struct ble_npl_event *ev)
{
    (void)ev;
    if (s_link_handle == BLE_HS_CONN_HANDLE_NONE && !ble_gap_adv_active()) {
        start_advertising();
    }
}

static bool is_owner(const ble_addr_t *peer)
{
    return s_has_owner && ble_addr_cmp(&s_owner, peer) == 0;
}

/* Hosts keep their keys after we drop ours, and macOS cannot recover from that
 * without the user forgetting the device. Only evict old bonds when the store
 * is full, and never the owner's.
 */
static int evict_oldest_bond(const ble_addr_t *keep)
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count;
    int rc = ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS);
    if (rc != 0) {
        ESP_LOGW(TAG, "could not list bonds: rc=%d", rc);
        return rc;
    }
    for (int i = 0; i < count; i++) {
        if (is_owner(&peers[i]) || ble_addr_cmp(&peers[i], keep) == 0) {
            continue;
        }
        rc = ble_gap_unpair(&peers[i]);
        if (rc != 0) {
            ESP_LOGW(TAG, "could not evict old bond: rc=%d", rc);
        }
        return rc;
    }
    ESP_LOGW(TAG, "bond storage is full and no old bond can be evicted");
    return BLE_HS_ESTORE_CAP;
}

static int store_status_cb(struct ble_store_status_event *event, void *arg)
{
    if (event->event_code != BLE_STORE_EVENT_OVERFLOW) {
        return ble_store_util_status_rr(event, arg);
    }
    switch (event->overflow.obj_type) {
    case BLE_STORE_OBJ_TYPE_OUR_SEC:
    case BLE_STORE_OBJ_TYPE_PEER_SEC:
        return evict_oldest_bond(&event->overflow.value->sec.peer_addr);
    case BLE_STORE_OBJ_TYPE_CCCD:
        return evict_oldest_bond(&event->overflow.value->cccd.peer_addr);
    case BLE_STORE_OBJ_TYPE_CSFC:
        return evict_oldest_bond(&event->overflow.value->csfc.peer_addr);
    default:
        return ble_store_util_status_rr(event, arg);
    }
}

static bool save_owner(const ble_addr_t *peer)
{
    /* A bond must already be stored before it can replace the current owner. */
    const struct ble_store_key_sec key = {.peer_addr = *peer};
    struct ble_store_value_sec bond;
    if (ble_store_read_our_sec(&key, &bond) != 0 || !bond.ltk_present) {
        ESP_LOGW(TAG, "pairing did not create a usable bond");
        return false;
    }

    esp_err_t err = nvs_set_blob(s_owner_store, OWNER_KEY, peer, sizeof(*peer));
    if (err == ESP_OK) {
        err = nvs_commit(s_owner_store);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not save pairing: %s", esp_err_to_name(err));
        return false;
    }

    s_owner = *peer;
    s_has_owner = true;
    return true;
}

static void terminate_link(void)
{
    int rc = ble_gap_terminate(s_link_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0 && rc != BLE_HS_EALREADY && rc != BLE_HS_ENOTCONN) {
        ESP_LOGW(TAG, "disconnect request failed: rc=%d", rc);
    }
}

static void set_pairing_open(bool open)
{
    if (s_pairing_open == open) {
        return;
    }
    s_pairing_open = open;
    if (open) {
        ble_npl_callout_reset(&s_pairing_timeout, pdMS_TO_TICKS(PAIRING_WINDOW_MS));
    } else {
        ble_npl_callout_stop(&s_pairing_timeout);
    }
    ESP_LOGI(TAG, "pairing window %s", open ? "opened" : "closed");
    if (s_pairing_cb) {
        s_pairing_cb(open);
    }
}

static void restart_advertising(void)
{
    stop_advertising();
    /* Start in a separate host event after stopping the old advertisement. */
    ble_npl_callout_reset(&s_adv_retry_callout, 1);
}

static void open_pairing_event_cb(struct ble_npl_event *event)
{
    (void)event;
    if (s_ota.active || s_pairing_open || !ble_hs_synced()) {
        return;
    }

    if (s_link_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "disconnect the current computer before entering pairing mode");
        return;
    }

    set_pairing_open(true);
    restart_advertising();
}

static void pairing_timeout_cb(struct ble_npl_event *event)
{
    (void)event;
    set_pairing_open(false);
    if (s_link_handle != BLE_HS_CONN_HANDLE_NONE) {
        if (!s_connected) {
            terminate_link();
        }
    } else {
        restart_advertising();
    }
}

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

    (void)voice_ble_request_fast_interval();

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
    ESP_LOGD(TAG, "OTA write offset=%" PRIu32 " len=%u total=%" PRIu32 "/%" PRIu32,
             offset, payload_len, s_ota.written, s_ota.image_size);
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
        ESP_LOGI(TAG, "OTA progress %" PRIu32 "/%" PRIu32 " (%d%%)",
                 s_ota.written, s_ota.image_size,
                 (int)(s_ota.written * 100 / s_ota.image_size));
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
    (void)attr_handle;
    (void)arg;

    if (!s_connected || conn_handle != s_conn_handle) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }

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

static void send_legacy_device_info_reply(void)
{
    if (!s_connected || !s_control_tx_subscribed || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *version = app_desc ? app_desc->version : "unknown";
    char json[192];
    const int len = snprintf(json, sizeof(json),
                             "{\"event\":\"device_info\",\"hardware\":\"%s\","
                             "\"firmware_version\":\"%s\",\"protocol_version\":%d}",
                             board_hardware_name(), version, VOICE_BLE_PROTOCOL_VERSION);
    if (len <= 0 || (size_t)len >= sizeof(json) || (size_t)len + 7 > ble_att_mtu(s_conn_handle)) {
        return;
    }
    uint8_t header[4] = {1, 0x10, (uint8_t)(len & 0xff), (uint8_t)(len >> 8)};
    struct os_mbuf *om = ble_hs_mbuf_from_flat(header, sizeof(header));
    if (!om) {
        return;
    }
    if (os_mbuf_append(om, json, (size_t)len) != 0) {
        os_mbuf_free_chain(om);
        return;
    }
    (void)ble_gatts_notify_custom(s_conn_handle, s_control_tx_attr_handle, om);
}

static int handle_control_chunk(const uint8_t *buf, uint16_t payload_len)
{
    if (payload_len < 3) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    const uint8_t msg_id = buf[0];
    const uint8_t chunk_index = buf[1];
    const uint8_t chunk_count = buf[2];
    const uint8_t *slice = buf + 3;
    const size_t slice_len = payload_len - 3;

    if (chunk_count < 2 || chunk_index >= chunk_count) {
        reset_rx_reasm();
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    const int64_t now = esp_timer_get_time();
    if (chunk_index == 0) {
        if (slice_len > VOICE_BLE_MAX_CONTROL_MESSAGE_BYTES) {
            reset_rx_reasm();
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        s_rx_reasm.active = true;
        s_rx_reasm.msg_id = msg_id;
        s_rx_reasm.next_index = 1;
        s_rx_reasm.count = chunk_count;
        s_rx_reasm.len = slice_len;
        s_rx_reasm.started_us = now;
        memcpy(s_rx_reasm.buf, slice, slice_len);
        return 0;
    }

    if (!s_rx_reasm.active || s_rx_reasm.msg_id != msg_id ||
        s_rx_reasm.next_index != chunk_index || s_rx_reasm.count != chunk_count ||
        (now - s_rx_reasm.started_us) > CONTROL_REASM_TIMEOUT_US ||
        s_rx_reasm.len + slice_len > VOICE_BLE_MAX_CONTROL_MESSAGE_BYTES) {
        reset_rx_reasm();
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    memcpy(s_rx_reasm.buf + s_rx_reasm.len, slice, slice_len);
    s_rx_reasm.len += slice_len;
    s_rx_reasm.next_index++;

    if (s_rx_reasm.next_index == s_rx_reasm.count) {
        s_rx_reasm.buf[s_rx_reasm.len] = '\0';
        s_rx_reasm.active = false;
        ESP_LOGD(TAG, "control rx(chunked) %s", s_rx_reasm.buf);
        if (s_control_cb) {
            s_control_cb(s_rx_reasm.buf);
        }
        reset_rx_reasm();
    }
    return 0;
}

static int control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (!s_connected || conn_handle != s_conn_handle) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > VOICE_BLE_MAX_CONTROL_MESSAGE_BYTES + 8) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t *buffer = malloc(len + 1);
    if (!buffer) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buffer, len, NULL);
    if (rc != 0) {
        free(buffer);
        return BLE_ATT_ERR_UNLIKELY;
    }
    buffer[len] = '\0';

    if (len >= 4 && buffer[0] == VOICE_BLE_PROTOCOL_VERSION) {
        const uint8_t type = buffer[1];
        const uint16_t payload_len = read_le16(&buffer[2]);
        if (4u + payload_len > len) {
            free(buffer);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (type == VOICE_BLE_TYPE_CONTROL_JSON) {
            if (payload_len > VOICE_BLE_MAX_CONTROL_MESSAGE_BYTES) {
                free(buffer);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            buffer[4 + payload_len] = '\0';
            const char *json = (const char *)&buffer[4];
            ESP_LOGD(TAG, "control rx %s", json);
            if (s_control_cb) {
                s_control_cb(json);
            }
            free(buffer);
            return 0;
        }
        if (type == VOICE_BLE_TYPE_CONTROL_CHUNK) {
            rc = handle_control_chunk(&buffer[4], payload_len);
            free(buffer);
            return rc;
        }
        free(buffer);
        return BLE_ATT_ERR_UNLIKELY;
    }

    /* Legacy unframed v1 client probe: respond with v1 device_info so old apps see our version. */
    if (buffer[0] == '{' && strstr((const char *)buffer, "\"get_device_info\"") != NULL) {
        ESP_LOGI(TAG, "legacy v1 get_device_info probe received");
        send_legacy_device_info_reply();
    }

    free(buffer);
    return 0;
}

static int stream_rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (!s_connected || conn_handle != s_conn_handle) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!s_stream_rx_cb) {
        return 0;
    }

    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0) {
        return 0;
    }
    uint8_t *buffer = malloc(len);
    if (!buffer) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, buffer, len, NULL) == 0) {
        s_stream_rx_cb(buffer, len);
    }
    free(buffer);
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

static int authorized_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (!s_connected || conn_handle != s_conn_handle) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }
    const uint8_t authorized = 1;
    return os_mbuf_append(ctxt->om, &authorized, sizeof(authorized)) == 0
        ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_stream_tx_uuid.u,
                .access_cb = notify_access_cb,
                .val_handle = &s_stream_tx_attr_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &s_control_tx_uuid.u,
                .access_cb = notify_access_cb,
                .val_handle = &s_control_tx_attr_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &s_control_rx_uuid.u,
                .access_cb = control_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = &s_ota_rx_uuid.u,
                .access_cb = ota_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = &s_ota_state_uuid.u,
                .access_cb = notify_access_cb,
                .val_handle = &s_ota_state_attr_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &s_authorized_uuid.u,
                .access_cb = authorized_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            {
                .uuid = &s_stream_rx_uuid.u,
                .access_cb = stream_rx_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {0},
        },
    },
    {0},
};

static void on_link_encrypted(uint16_t conn_handle, int status)
{
    if (conn_handle != s_link_handle) {
        return;
    }
    struct ble_gap_conn_desc desc;
    const bool secure = ble_gap_conn_find(conn_handle, &desc) == 0 &&
                        desc.sec_state.encrypted && desc.sec_state.bonded;
    if (status == BLE_HS_ETIMEOUT && s_connected && secure && is_owner(&desc.peer_id_addr)) {
        ESP_LOGD(TAG, "security request expired on an already authorized link");
        return;
    }
    if (status != 0 || !secure) {
        ESP_LOGW(TAG, "link security failed: status=%d", status);
        terminate_link();
        return;
    }

    if (!is_owner(&desc.peer_id_addr) && (!s_pairing_open || !save_owner(&desc.peer_id_addr))) {
        ESP_LOGW(TAG, "encrypted peer is not authorized");
        terminate_link();
        return;
    }
    if (s_connected) {
        return;
    }

    s_conn_handle = conn_handle;
    s_connected = true;
    ble_npl_callout_stop(&s_link_security);
    set_pairing_open(false);
    ESP_LOGI(TAG, "authorized connection handle=%u", conn_handle);
    int dle_rc = ble_gap_set_data_len(conn_handle, BLE_HCI_SUGG_DEF_DATALEN_TX_OCTETS_MAX,
                                     BLE_HCI_SUGG_DEF_DATALEN_TX_TIME_MAX);
    if (dle_rc != 0) {
        ESP_LOGD(TAG, "data length update request failed rc=%d", dle_rc);
    }
    if (ble_att_mtu(conn_handle) == BLE_ATT_MTU_DFLT) {
        int rc = ble_gattc_exchange_mtu(conn_handle, NULL, NULL);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "mtu exchange request failed rc=%d", rc);
        }
    }
    if (s_connection_cb) {
        s_connection_cb(true);
    }
}

int __real_ble_hs_hci_util_set_data_len(uint16_t conn_handle, uint16_t tx_octets, uint16_t tx_time);

int __wrap_ble_hs_hci_util_set_data_len(uint16_t conn_handle, uint16_t tx_octets, uint16_t tx_time)
{
    /* ESP-IDF's NimBLE host requests DLE from its connection callback.
     * Defer that request until the saved host has restored encryption. */
    if (!s_connected || conn_handle != s_conn_handle) {
        return 0;
    }
    return __real_ble_hs_hci_util_set_data_len(conn_handle, tx_octets, tx_time);
}

static bool request_link_security(uint16_t conn_handle)
{
    int rc = ble_gap_security_initiate(conn_handle);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        return true;
    }
    ESP_LOGW(TAG, "security request failed: rc=%d", rc);
    terminate_link();
    return false;
}

static void link_security_cb(struct ble_npl_event *event)
{
    (void)event;
    struct ble_gap_conn_desc desc;
    if (s_connected || ble_gap_conn_find(s_link_handle, &desc) != 0) {
        return;
    }
    if (desc.sec_state.encrypted && desc.sec_state.bonded) {
        on_link_encrypted(s_link_handle, 0);
    } else {
        request_link_security(s_link_handle);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            start_advertising();
            return 0;
        }

        s_link_handle = event->connect.conn_handle;
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(s_link_handle, &desc) != 0 ||
            (!s_pairing_open && !is_owner(&desc.peer_id_addr))) {
            ESP_LOGI(TAG, "rejected connection outside the pairing window");
            terminate_link();
            return 0;
        }

        if (desc.sec_state.encrypted && desc.sec_state.bonded) {
            on_link_encrypted(s_link_handle, 0);
        } else {
            ble_npl_callout_reset(&s_link_security, pdMS_TO_TICKS(LINK_SECURITY_GRACE_MS));
        }
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        on_link_encrypted(event->enc_change.conn_handle, event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (s_pairing_open &&
            ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0 &&
            ble_store_util_delete_peer(&desc.peer_id_addr) == 0) {
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        if (event->disconnect.conn.conn_handle != s_link_handle) {
            return 0;
        }
        ble_npl_callout_stop(&s_link_security);
        const bool was_connected = s_connected;
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
        s_stream_tx_subscribed = false;
        s_control_tx_subscribed = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_link_handle = BLE_HS_CONN_HANDLE_NONE;
        s_itvl_target = CONN_ITVL_NONE;
        s_itvl_update_pending = false;
        s_itvl_update_in_flight = false;
        reset_rx_reasm();
        if (s_control_tx_lock) {
            xSemaphoreTake(s_control_tx_lock, portMAX_DELAY);
            s_tx_msg_id = 0;
            s_event_seq = 0;
            xSemaphoreGive(s_control_tx_lock);
        }
        start_advertising();
        if (was_connected && s_connection_cb) {
            s_connection_cb(false);
        }
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (s_link_handle != event->subscribe.conn_handle) {
            return 0;
        }
        if (event->subscribe.attr_handle == s_stream_tx_attr_handle) {
            s_stream_tx_subscribed = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_control_tx_attr_handle) {
            s_control_tx_subscribed = event->subscribe.cur_notify;
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        int find_rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        if (find_rc == 0) {
            ESP_LOGI(TAG, "conn updated: status=%d interval=%u latency=%u timeout=%u",
                     event->conn_update.status,
                     desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
        } else {
            ESP_LOGI(TAG, "conn updated: status=%d (desc unavailable)",
                     event->conn_update.status);
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        ESP_LOGD(TAG, "mtu=%u", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void start_advertising(void)
{
    if (s_link_handle != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active() ||
        (!s_pairing_open && !s_has_owner)) {
        return;
    }

    const bool pairing_with_owner = s_pairing_open && s_has_owner;
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_BREDR_UNSUP | (s_pairing_open ? BLE_HS_ADV_F_DISC_GEN : 0);
    fields.uuids128 = s_pairing_open ? &s_pairing_adv_uuid : &s_reconnect_adv_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = !pairing_with_owner;

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
    if (pairing_with_owner) {
        rsp_fields.uuids128 = &s_reconnect_adv_uuid;
        rsp_fields.num_uuids128 = 1;
    }
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set scan response failed rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(s_pairing_open ? 60 : 200);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(s_pairing_open ? 120 : 320);

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "start advertising failed rc=%d, will retry in %d ms",
                 rc, ADV_RETRY_DELAY_MS);
        ble_npl_callout_reset(&s_adv_retry_callout,
                              pdMS_TO_TICKS(ADV_RETRY_DELAY_MS));
        return;
    }

    ESP_LOGI(TAG, "advertising as %s for %s", s_device_name,
             s_pairing_open ? "pairing" : "reconnection");
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

    if (!s_has_owner && !s_pairing_open) {
        set_pairing_open(true);
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

static esp_err_t init_device_uuid(void)
{
    const char *key = "device_uuid";
    nvs_handle_t store;
    ESP_RETURN_ON_ERROR(nvs_open("voice_identity", NVS_READWRITE, &store),
                        TAG, "device identity storage open failed");

    uint8_t uuid[16];
    size_t size = sizeof(uuid);
    esp_err_t err = nvs_get_blob(store, key, uuid, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        esp_fill_random(uuid, sizeof(uuid));
        uuid[6] = (uuid[6] & 0x0f) | 0x40;
        uuid[8] = (uuid[8] & 0x3f) | 0x80;
        err = nvs_set_blob(store, key, uuid, sizeof(uuid));
        if (err == ESP_OK) {
            err = nvs_commit(store);
        }
    } else if (err == ESP_OK && size != sizeof(uuid)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(store);
    if (err != ESP_OK) {
        return err;
    }

    snprintf(s_device_uuid, sizeof(s_device_uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
             uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return ESP_OK;
}

esp_err_t voice_ble_init(void)
{
    ESP_RETURN_ON_ERROR(init_device_identity(), TAG, "device identity init failed");

    s_control_tx_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_control_tx_lock != NULL, ESP_ERR_NO_MEM, TAG, "create control tx lock");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init failed");

    ESP_RETURN_ON_ERROR(nvs_open("voice_pairing", NVS_READWRITE, &s_owner_store),
                        TAG, "pairing storage open failed");
    size_t owner_size = sizeof(s_owner);
    err = nvs_get_blob(s_owner_store, OWNER_KEY, &s_owner, &owner_size);
    s_has_owner = err == ESP_OK && owner_size == sizeof(s_owner);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "could not read pairing owner: %s", esp_err_to_name(err));
    }

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init failed");

    err = init_device_uuid();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device UUID unavailable: %s", esp_err_to_name(err));
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = store_status_cb;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();

    int rc = ble_svc_gap_device_name_set(s_device_name);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "set device name failed rc=%d", rc);

    rc = ble_gatts_count_cfg(s_gatt_services);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "count gatt failed rc=%d", rc);
    rc = ble_gatts_add_svcs(s_gatt_services);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "add gatt failed rc=%d", rc);

    ble_npl_callout_init(&s_adv_retry_callout, nimble_port_get_dflt_eventq(),
                         adv_retry_callout_cb, NULL);
    ble_npl_event_init(&s_open_pairing_event, open_pairing_event_cb, NULL);
    ble_npl_callout_init(&s_pairing_timeout, nimble_port_get_dflt_eventq(), pairing_timeout_cb, NULL);
    ble_npl_callout_init(&s_link_security, nimble_port_get_dflt_eventq(), link_security_cb, NULL);

    nimble_port_freertos_init(nimble_host_task);
    s_ble_initialized = true;
    ESP_LOGI(TAG, "BLE v2 initialized as %s", s_device_name);
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

const char *voice_ble_device_uuid(void)
{
    return s_device_uuid;
}

void voice_ble_set_connection_callback(voice_ble_connection_cb_t callback)
{
    s_connection_cb = callback;
}

void voice_ble_set_pairing_callback(voice_ble_pairing_cb_t callback)
{
    s_pairing_cb = callback;
}

void voice_ble_open_pairing(void)
{
    if (s_ble_initialized && !voice_ble_has_active_link()) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_open_pairing_event);
    }
}

void voice_ble_set_control_callback(voice_ble_control_cb_t callback)
{
    s_control_cb = callback;
}

void voice_ble_set_stream_rx_callback(voice_ble_stream_rx_cb_t callback)
{
    s_stream_rx_cb = callback;
}

void voice_ble_set_ota_callback(voice_ble_ota_cb_t callback)
{
    s_ota_cb = callback;
}

bool voice_ble_is_connected(void)
{
    return s_connected;
}

bool voice_ble_has_active_link(void)
{
    return s_link_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool voice_ble_control_ready(void)
{
    return !s_pairing_open && s_connected && s_control_tx_subscribed;
}

bool voice_ble_is_ready(void)
{
    return !s_pairing_open && s_connected && s_stream_tx_subscribed && s_control_tx_subscribed;
}

bool voice_ble_ota_is_active(void)
{
    return s_ota.active;
}

static esp_err_t notify_with_retry(uint16_t attr_handle, const uint8_t *header, size_t header_len,
                                   const uint8_t *payload, size_t payload_len)
{
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_pairing_open) {
            return ESP_ERR_INVALID_STATE;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(header, header_len);
        if (!om) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (payload_len > 0 && os_mbuf_append(om, payload, payload_len) != 0) {
            os_mbuf_free_chain(om);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const int rc = ble_gatts_notify_custom(s_conn_handle, attr_handle, om);
        if (rc == 0) {
            record_tx_activity();
            return ESP_OK;
        }
        if (rc != BLE_HS_ENOMEM) {
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t voice_ble_send_control_json(const char *json)
{
    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!voice_ble_control_ready() || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t json_len = strlen(json);
    if (json_len == 0 || json_len > VOICE_BLE_MAX_CONTROL_MESSAGE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_control_tx_lock, portMAX_DELAY);
    const uint16_t mtu = ble_att_mtu(s_conn_handle);
    const size_t max_att = mtu > 3 ? (size_t)(mtu - 3) : 20;
    esp_err_t err = ESP_OK;

    if (4 + json_len <= max_att) {
        const uint8_t header[4] = {
            VOICE_BLE_PROTOCOL_VERSION,
            VOICE_BLE_TYPE_CONTROL_JSON,
            (uint8_t)(json_len & 0xff),
            (uint8_t)(json_len >> 8),
        };
        err = notify_with_retry(s_control_tx_attr_handle, header, sizeof(header),
                                (const uint8_t *)json, json_len);
    } else {
        const size_t max_slice = max_att > 7 ? max_att - 7 : 13;
        const size_t chunk_count = (json_len + max_slice - 1) / max_slice;
        if (chunk_count < 2 || chunk_count > 255) {
            xSemaphoreGive(s_control_tx_lock);
            return ESP_ERR_INVALID_SIZE;
        }
        const uint8_t msg_id = s_tx_msg_id++;
        size_t offset = 0;
        for (size_t i = 0; i < chunk_count; ++i) {
            const size_t slice_len = MIN(max_slice, json_len - offset);
            const uint16_t payload_len = (uint16_t)(3 + slice_len);
            const uint8_t header[7] = {
                VOICE_BLE_PROTOCOL_VERSION,
                VOICE_BLE_TYPE_CONTROL_CHUNK,
                (uint8_t)(payload_len & 0xff),
                (uint8_t)(payload_len >> 8),
                msg_id,
                (uint8_t)i,
                (uint8_t)chunk_count,
            };
            err = notify_with_retry(s_control_tx_attr_handle, header, sizeof(header),
                                    (const uint8_t *)json + offset, slice_len);
            if (err != ESP_OK) {
                break;
            }
            offset += slice_len;
        }
    }

    xSemaphoreGive(s_control_tx_lock);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "control tx (%uB) %s", (unsigned)json_len, json);
    } else {
        ESP_LOGW(TAG, "control tx failed (%s) len=%u", esp_err_to_name(err), (unsigned)json_len);
    }
    return err;
}

esp_err_t voice_ble_send_event(const char *event_name, uint64_t timestamp_ms, const char *data_json)
{
    if (!event_name || !data_json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!voice_ble_control_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_control_tx_lock, portMAX_DELAY);
    const uint32_t seq = ++s_event_seq;
    xSemaphoreGive(s_control_tx_lock);

    char buf[384];
    const int len = snprintf(buf, sizeof(buf),
                             "{\"event\":\"%s\",\"seq\":%" PRIu32
                             ",\"timestamp_ms\":%" PRIu64 ",\"data\":%s}",
                             event_name, seq, timestamp_ms, data_json);
    if (len <= 0 || (size_t)len >= sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return voice_ble_send_control_json(buf);
}

esp_err_t voice_ble_send_audio(uint16_t stream_id, uint32_t seq, uint32_t timestamp_ms,
                               uint8_t flags, const uint8_t *payload, size_t len)
{
    if (!s_connected || !s_stream_tx_subscribed || s_pairing_open ||
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > UINT16_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t header[18] = {
        VOICE_BLE_PROTOCOL_VERSION,
        VOICE_BLE_TYPE_AUDIO_IN,
        18,
        0,
        (uint8_t)(stream_id & 0xff),
        (uint8_t)(stream_id >> 8),
        flags,
        0,
        (uint8_t)(seq & 0xff),
        (uint8_t)((seq >> 8) & 0xff),
        (uint8_t)((seq >> 16) & 0xff),
        (uint8_t)((seq >> 24) & 0xff),
        (uint8_t)(timestamp_ms & 0xff),
        (uint8_t)((timestamp_ms >> 8) & 0xff),
        (uint8_t)((timestamp_ms >> 16) & 0xff),
        (uint8_t)((timestamp_ms >> 24) & 0xff),
        (uint8_t)(len & 0xff),
        (uint8_t)((len >> 8) & 0xff),
    };

    struct os_mbuf *om = ble_hs_mbuf_from_flat(header, sizeof(header));
    if (!om) {
        ESP_LOGW(TAG, "audio tx seq=%" PRIu32 " mbuf alloc failed", seq);
        return ESP_ERR_NO_MEM;
    }
    if (len > 0) {
        int rc = os_mbuf_append(om, payload, len);
        if (rc != 0) {
            os_mbuf_free_chain(om);
            return ESP_FAIL;
        }
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_stream_tx_attr_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "audio tx seq=%" PRIu32 " notify failed rc=%d", seq, rc);
        return rc == BLE_HS_ENOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    record_tx_activity();

    ESP_LOGD(TAG, "audio tx stream=%u seq=%" PRIu32 " flags=0x%02x len=%u",
             stream_id, seq, flags, (unsigned)len);
    return ESP_OK;
}

esp_err_t voice_ble_send_imu(uint16_t stream_id, uint16_t seq, uint32_t timestamp_ms,
                             const uint8_t *samples, size_t sample_bytes)
{
    if (!s_connected || !s_stream_tx_subscribed || s_pairing_open ||
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_bytes != 6 && sample_bytes != 12) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Reserve mbuf headroom for control_tx, input, and audio_in under radio congestion. */
    if (os_msys_num_free() < IMU_MIN_FREE_MBUFS) {
        return ESP_ERR_NO_MEM;
    }

    const uint16_t payload_len = (uint16_t)(8 + sample_bytes);
    uint8_t frame[24] = {
        VOICE_BLE_PROTOCOL_VERSION,
        VOICE_BLE_TYPE_IMU,
        (uint8_t)(payload_len & 0xff),
        (uint8_t)(payload_len >> 8),
        (uint8_t)(stream_id & 0xff),
        (uint8_t)(stream_id >> 8),
        (uint8_t)(seq & 0xff),
        (uint8_t)(seq >> 8),
        (uint8_t)(timestamp_ms & 0xff),
        (uint8_t)((timestamp_ms >> 8) & 0xff),
        (uint8_t)((timestamp_ms >> 16) & 0xff),
        (uint8_t)((timestamp_ms >> 24) & 0xff),
    };
    memcpy(frame + 12, samples, sample_bytes);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, 12 + sample_bytes);
    if (!om) {
        return ESP_ERR_NO_MEM;
    }
    if (ble_gatts_notify_custom(s_conn_handle, s_stream_tx_attr_handle, om) != 0) {
        return ESP_FAIL;
    }
    record_tx_activity();
    return ESP_OK;
}

esp_err_t voice_ble_send_input(uint16_t stream_id, uint16_t seq, uint32_t timestamp_ms,
                               uint32_t buttons_mask)
{
    if (!s_connected || !s_stream_tx_subscribed || s_pairing_open ||
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t frame[16] = {
        VOICE_BLE_PROTOCOL_VERSION,
        VOICE_BLE_TYPE_INPUT,
        12,
        0,
        (uint8_t)(stream_id & 0xff),
        (uint8_t)(stream_id >> 8),
        (uint8_t)(seq & 0xff),
        (uint8_t)(seq >> 8),
        (uint8_t)(timestamp_ms & 0xff),
        (uint8_t)((timestamp_ms >> 8) & 0xff),
        (uint8_t)((timestamp_ms >> 16) & 0xff),
        (uint8_t)((timestamp_ms >> 24) & 0xff),
        (uint8_t)(buttons_mask & 0xff),
        (uint8_t)((buttons_mask >> 8) & 0xff),
        (uint8_t)((buttons_mask >> 16) & 0xff),
        (uint8_t)((buttons_mask >> 24) & 0xff),
    };
    return notify_with_retry(s_stream_tx_attr_handle, frame, sizeof(frame), NULL, 0);
}

esp_err_t voice_ble_send_nop(void)
{
    if (!s_connected || !s_stream_tx_subscribed || s_pairing_open ||
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Fire-and-forget: if TX buffers are busy, the radio is already awake. */
    if (os_msys_num_free() < IMU_MIN_FREE_MBUFS) {
        return ESP_ERR_NO_MEM;
    }

    const uint8_t frame[4] = {
        VOICE_BLE_PROTOCOL_VERSION,
        VOICE_BLE_TYPE_NOP,
        0,
        0,
    };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, sizeof(frame));
    if (!om) {
        return ESP_ERR_NO_MEM;
    }
    const int rc = ble_gatts_notify_custom(s_conn_handle, s_stream_tx_attr_handle, om);
    if (rc != 0) {
        return rc == BLE_HS_ENOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    record_tx_activity();
    return ESP_OK;
}

uint32_t voice_ble_last_tx_ms(void)
{
    return atomic_load(&s_last_tx_ms);
}

static void conn_params_response_cb(uint16_t conn_handle, int status, void *arg)
{
    (void)arg;
    if (conn_handle != s_conn_handle) {
        return;
    }
    s_itvl_update_in_flight = false;
    if (status != 0) {
        ESP_LOGW(TAG, "conn params request not accepted status=%d", status);
    } else {
        ESP_LOGI(TAG, "conn params request accepted");
    }
    if (s_itvl_update_pending) {
        s_itvl_update_pending = false;
        const conn_itvl_target_t pending = s_itvl_target;
        s_itvl_target = CONN_ITVL_NONE;
        if (pending == CONN_ITVL_FAST) {
            (void)voice_ble_request_fast_interval();
        } else if (pending == CONN_ITVL_SLOW) {
            (void)voice_ble_request_slow_interval();
        }
    }
}

static esp_err_t request_conn_params(conn_itvl_target_t target,
                                     struct ble_l2cap_sig_update_params *params,
                                     const char *label)
{
    if (s_pairing_open || !s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_itvl_target == target) {
        return ESP_OK;
    }
    s_itvl_target = target;
    if (s_itvl_update_in_flight) {
        s_itvl_update_pending = true;
        ESP_LOGD(TAG, "%s interval deferred (update in progress)", label);
        return ESP_OK;
    }
    int rc = ble_l2cap_sig_update(s_conn_handle, params, conn_params_response_cb, NULL);
    if (rc != 0) {
        s_itvl_target = CONN_ITVL_NONE;
        ESP_LOGW(TAG, "%s interval request failed rc=%d", label, rc);
        return ESP_FAIL;
    }
    s_itvl_update_pending = false;
    s_itvl_update_in_flight = true;
    ESP_LOGI(TAG, "requested %s conn interval %u-%ums latency=%u", label,
             params->itvl_min * 5 / 4, params->itvl_max * 5 / 4, params->slave_latency);
    return ESP_OK;
}

esp_err_t voice_ble_request_fast_interval(void)
{
    struct ble_l2cap_sig_update_params params = {
        .itvl_min = 12,   /* 15 ms */
        .itvl_max = 24,   /* 30 ms */
        .slave_latency = 0,
        .timeout_multiplier = 400,  /* 4 s */
    };
    return request_conn_params(CONN_ITVL_FAST, &params, "fast");
}

void voice_ble_check_fast_interval(void)
{
    struct ble_gap_conn_desc desc;
    if (!s_connected || ble_gap_conn_find(s_conn_handle, &desc) != 0 ||
        (desc.conn_itvl <= 24 && desc.conn_latency == 0)) {
        return;
    }
    const struct ble_gap_upd_params params = {
        .itvl_min = 12, .itvl_max = 24, .latency = 0, .supervision_timeout = 400,
    };
    int rc = ble_gap_update_params(s_conn_handle, &params);
    ESP_LOGI(TAG, "fast link retry interval=%u latency=%u update=%d",
             desc.conn_itvl, desc.conn_latency, rc);
}

esp_err_t voice_ble_request_slow_interval(void)
{
    /* Mouse-style BLE parameters, Apple-compliant:
     * interval_min >= 15ms, latency <= 30,
     * interval_max * (latency + 1) <= 2s, timeout <= 6s.
     * Peripheral uplink (buttons, Opus audio, IMU) skips slave_latency
     * immediately at the negotiated interval while idle radio sleeps. */
    struct ble_l2cap_sig_update_params params = {
        .itvl_min = CONFIG_VOICE_STICK_BLE_ITVL_MIN_UNITS,
        .itvl_max = CONFIG_VOICE_STICK_BLE_ITVL_MAX_UNITS,
        .slave_latency = CONFIG_VOICE_STICK_BLE_SLAVE_LATENCY,
        .timeout_multiplier = 600,  /* 6 s */
    };
    return request_conn_params(CONN_ITVL_SLOW, &params, "normal");
}
