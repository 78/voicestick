#include "voice_ble.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
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
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "board.h"

static const char *TAG = "voice_ble";

#define OTA_PROGRESS_NOTIFY_BYTES (32 * 1024)
#define PAIRING_WINDOW_MS (60 * 1000)
#define BONDED_SECURITY_GRACE_MS 1500

static atomic_bool s_connected;
static bool s_audio_subscribed;
static atomic_bool s_state_subscribed;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static atomic_uint_least16_t s_link_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static uint16_t s_audio_attr_handle;
static uint16_t s_state_attr_handle;
static uint16_t s_ota_state_attr_handle;
static char s_device_id[5] = "0000";
static char s_device_name[8] = VOICE_BLE_DEVICE_NAME_PREFIX "-0000";
static char s_device_uuid[37];
static voice_ble_connection_cb_t s_connection_cb;
static voice_ble_pairing_cb_t s_pairing_cb;
static voice_ble_control_cb_t s_control_cb;
static voice_ble_ota_cb_t s_ota_cb;
static atomic_bool s_ble_initialized;

/* The NimBLE host task owns pairing changes and bond storage. */
static atomic_bool s_pairing_open;
static bool s_has_owner;
static ble_addr_t s_owner;
static nvs_handle_t s_owner_store;
static const char *OWNER_KEY = "owner";
static struct ble_npl_event s_open_pairing_event;
static struct ble_npl_callout s_pairing_timeout;
static struct ble_npl_callout s_bonded_security;

typedef enum {
    CONN_ITVL_NONE,
    CONN_ITVL_FAST,
    CONN_ITVL_SLOW,
} conn_itvl_target_t;

static conn_itvl_target_t s_itvl_target;
static bool s_itvl_update_pending;
static bool s_itvl_update_in_flight;
static bool s_device_info_pending;
static bool s_host_commanded;

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
static const ble_uuid128_t s_pairing_adv_uuid =
    BLE_UUID128_INIT(0x06, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_reconnect_adv_uuid =
    BLE_UUID128_INIT(0x07, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_authorized_uuid =
    BLE_UUID128_INIT(0x08, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
                     0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);

void ble_store_config_init(void);

static void start_advertising(void);
static void stop_advertising(void);
static esp_err_t send_state_json(const char *json);
static esp_err_t send_device_info(void);
static void announce_state_subscription(void);
static struct ble_npl_callout s_adv_retry_callout;
#define ADV_RETRY_DELAY_MS 1000

#ifdef CONFIG_VOICE_STICK_BLE_BATTERY_STATUS
typedef struct {
    int percent;
    bool charging;
    bool usb_powered;
} battery_status_t;

/* The app task supplies the latest sample. NimBLE tracks what it sent. */
static portMUX_TYPE s_battery_status_lock = portMUX_INITIALIZER_UNLOCKED;
static battery_status_t s_battery_status = {.percent = -1};
static battery_status_t s_last_battery_report = {.percent = -1};
static struct ble_npl_event s_battery_status_event;

static void report_battery_status(void)
{
    if (!s_state_subscribed) {
        return;
    }

    portENTER_CRITICAL(&s_battery_status_lock);
    const battery_status_t status = s_battery_status;
    portEXIT_CRITICAL(&s_battery_status_lock);
    if (status.percent < 0) {
        return;
    }

    if (status.percent == s_last_battery_report.percent &&
        status.charging == s_last_battery_report.charging &&
        status.usb_powered == s_last_battery_report.usb_powered) {
        return;
    }

    char json[96];
    const int len = snprintf(json, sizeof(json),
                             "{\"event\":\"battery_status\",\"percent\":%d,"
                             "\"charging\":%s,\"usb_powered\":%s}",
                             status.percent, status.charging ? "true" : "false",
                             status.usb_powered ? "true" : "false");
    /* ATT notification overhead plus the VoiceStick state frame header. */
    if (len + 3 + 4 > ble_att_mtu(s_conn_handle) || send_state_json(json) != ESP_OK) {
        s_last_battery_report.percent = -1;
        return;
    }

    s_last_battery_report = status;
}

static void battery_status_event_cb(struct ble_npl_event *event)
{
    (void)event;
    report_battery_status();
}

void voice_ble_update_battery_status(int percent, bool charging, bool usb_powered)
{
    if (percent < 0 || percent > 100) {
        return;
    }

    portENTER_CRITICAL(&s_battery_status_lock);
    s_battery_status = (battery_status_t){percent, charging, usb_powered};
    portEXIT_CRITICAL(&s_battery_status_lock);

    if (s_state_subscribed) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_battery_status_event);
    }
}
#endif

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

    /* On C3 with ESP-IDF c43d665a96c, terminating the old link here hung in
     * API_vhci_host_send_packet while waiting for a controller semaphore.
     * Require the computer to disconnect before opening the pairing window.
     */
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
    voice_ble_request_slow_interval();
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

    voice_ble_request_fast_interval();

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
    voice_ble_request_slow_interval();
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

    char buffer[512] = {0};
    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    const uint16_t copy_len = MIN(len, sizeof(buffer) - 1);
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buffer, copy_len, NULL);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    ESP_LOGD(TAG, "control %s", buffer);
    if (s_control_cb) {
        s_control_cb(buffer);
    }
    /* A bonded central gets its subscription restored at encryption, before
     * its app listens, and Windows drops notifications nobody listens to.
     * The first command shows the app is listening.
     */
    if (!s_host_commanded) {
        voice_ble_send_device_snapshot();
    }
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
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
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
            {0},
        },
    },
    {0},
};

static void announce_state_subscription(void)
{
    if (!s_connected || !s_state_subscribed) {
        return;
    }
    esp_err_t rc = send_device_info();
    /* A bonded central restores its subscription before the MTU exchange
     * finishes, and device_info does not fit in the default MTU.
     */
    s_device_info_pending = rc == ESP_ERR_INVALID_SIZE;
    if (s_device_info_pending) {
        ESP_LOGD(TAG, "device_info deferred until the MTU exchange");
    } else if (rc != ESP_OK) {
        ESP_LOGW(TAG, "device_info send failed err=0x%x", rc);
    }
#ifdef CONFIG_VOICE_STICK_BLE_BATTERY_STATUS
    report_battery_status();
#endif
}

void voice_ble_send_device_snapshot(void)
{
    s_host_commanded = true;
#ifdef CONFIG_VOICE_STICK_BLE_BATTERY_STATUS
    /* A restored subscription can accept a notification before the app listens. */
    s_last_battery_report.percent = -1;
#endif
    announce_state_subscription();
}

static void on_link_encrypted(uint16_t conn_handle, int status)
{
    if (conn_handle != s_link_handle) {
        return;
    }
    struct ble_gap_conn_desc desc;
    const bool secure = ble_gap_conn_find(conn_handle, &desc) == 0 &&
                        desc.sec_state.encrypted && desc.sec_state.bonded;
    if (status == BLE_HS_ETIMEOUT && s_connected && secure && is_owner(&desc.peer_id_addr)) {
        /* The central may have restored encryption before receiving our Security Request. */
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
    ble_npl_callout_stop(&s_bonded_security);
    set_pairing_open(false);
    ESP_LOGI(TAG, "authorized connection handle=%u", conn_handle);
    int dle_rc = ble_gap_set_data_len(conn_handle, BLE_HCI_SUGG_DEF_DATALEN_TX_OCTETS_MAX,
                                     BLE_HCI_SUGG_DEF_DATALEN_TX_TIME_MAX);
    if (dle_rc != 0) {
        ESP_LOGD(TAG, "data length update request failed rc=%d", dle_rc);
    }
    /* Keep the peripheral's MTU request out of the security handshake. */
    if (ble_att_mtu(conn_handle) == BLE_ATT_MTU_DFLT) {
        int rc = ble_gattc_exchange_mtu(conn_handle, NULL, NULL);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "mtu exchange request failed rc=%d", rc);
        }
    }
    if (s_connection_cb) {
        s_connection_cb(true);
    }
    announce_state_subscription();
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

static void bonded_security_cb(struct ble_npl_event *event)
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
        } else if (is_owner(&desc.peer_id_addr)) {
            /* A bonded central can start restoring encryption on its own. */
            ble_npl_callout_reset(&s_bonded_security, pdMS_TO_TICKS(BONDED_SECURITY_GRACE_MS));
        } else if (!request_link_security(s_link_handle)) {
            return 0;
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
        ble_npl_callout_stop(&s_bonded_security);
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
        s_audio_subscribed = false;
        s_state_subscribed = false;
#ifdef CONFIG_VOICE_STICK_BLE_BATTERY_STATUS
        s_last_battery_report.percent = -1;
#endif
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_link_handle = BLE_HS_CONN_HANDLE_NONE;
        s_itvl_target = CONN_ITVL_NONE;
        s_itvl_update_pending = false;
        s_itvl_update_in_flight = false;
        s_device_info_pending = false;
        s_host_commanded = false;
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
        if (event->subscribe.attr_handle == s_audio_attr_handle) {
            s_audio_subscribed = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_state_attr_handle) {
            s_state_subscribed = event->subscribe.cur_notify;
#ifdef CONFIG_VOICE_STICK_BLE_BATTERY_STATUS
            s_last_battery_report.percent = -1;
#endif
            announce_state_subscription();
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
        if (s_device_info_pending) {
            announce_state_subscription();
            return 0;
        }
#ifdef CONFIG_VOICE_STICK_BLE_BATTERY_STATUS
        report_battery_status();
#endif
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
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
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
    // The paired host can still find the device while pairing is available.
    // A second 128-bit UUID does not fit in the primary advertising packet.
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

    /* A device without an owner cannot be taken from anyone, so skip the button hold. */
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

    /* The Bluetooth controller is enabled before the first UUID needs entropy. */
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
    ble_npl_callout_init(&s_bonded_security, nimble_port_get_dflt_eventq(), bonded_security_cb, NULL);
#ifdef CONFIG_VOICE_STICK_BLE_BATTERY_STATUS
    ble_npl_event_init(&s_battery_status_event, battery_status_event_cb, NULL);
#endif

    nimble_port_freertos_init(nimble_host_task);
    s_ble_initialized = true;
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

bool voice_ble_is_ready(void)
{
    return !s_pairing_open && s_connected && s_audio_subscribed && s_state_subscribed;
}

bool voice_ble_ota_is_active(void)
{
    return s_ota.active;
}

esp_err_t voice_ble_send_audio(uint32_t session_id, uint32_t seq, uint8_t flags,
                               const uint8_t *opus_payload, size_t len)
{
    if (!voice_ble_is_ready() || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > UINT16_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

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
        ESP_LOGW(TAG, "tx seq=%" PRIu32 " mbuf alloc failed", seq);
        return ESP_ERR_NO_MEM;
    }

    if (len > 0) {
        int rc = os_mbuf_append(om, opus_payload, len);
        if (rc != 0) {
            os_mbuf_free_chain(om);
            ESP_LOGW(TAG, "tx seq=%" PRIu32 " mbuf append failed rc=%d", seq, rc);
            return ESP_FAIL;
        }
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_audio_attr_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "tx seq=%" PRIu32 " notify failed rc=%d", seq, rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "tx seq=%" PRIu32 " %u bytes, sram: %" PRIu32 " bytes",
             seq, (unsigned)(sizeof(header) + len), esp_get_free_internal_heap_size());
    return ESP_OK;
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
        if (s_itvl_target == CONN_ITVL_FAST) {
            voice_ble_request_fast_interval();
        } else if (s_itvl_target == CONN_ITVL_SLOW) {
            voice_ble_request_slow_interval();
        }
    }
}

/* Windows controllers advertise the LL connection parameters request procedure
 * but some never answer it, and the link then drops after the 40 s LL response
 * timeout. The L2CAP request is answered by the host, and an unanswered one
 * only times out the request.
 */
static esp_err_t request_conn_params(conn_itvl_target_t target,
                                     struct ble_l2cap_sig_update_params *params,
                                     const char *label)
{
    if (s_pairing_open || !s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_itvl_target = target;
    if (s_itvl_update_in_flight) {
        s_itvl_update_pending = true;
        ESP_LOGD(TAG, "%s interval deferred (update in progress)", label);
        return ESP_OK;
    }
    int rc = ble_l2cap_sig_update(s_conn_handle, params, conn_params_response_cb, NULL);
    if (rc != 0) {
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
        .itvl_min = 12,   // 15ms
        .itvl_max = 24,   // 30ms
        .slave_latency = 0,
        .timeout_multiplier = 200,  // 2s
    };
    return request_conn_params(CONN_ITVL_FAST, &params, "fast");
}

esp_err_t voice_ble_request_slow_interval(void)
{
    /* Apple caps interval * (latency + 1) at 2s and the supervision timeout at 6s. */
    struct ble_l2cap_sig_update_params params = {
        .itvl_min = 40,   // 50ms
        .itvl_max = 160,  // 200ms
        .slave_latency = 8,
        .timeout_multiplier = 600,  // 6s
    };
    return request_conn_params(CONN_ITVL_SLOW, &params, "slow");
}

static esp_err_t send_state_json(const char *json)
{
    if (s_pairing_open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_connected || !s_state_subscribed || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "send_state_json gated: connected=%d state_sub=%d conn=%u",
                 s_connected, s_state_subscribed, s_conn_handle);
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

static esp_err_t send_device_info(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *version = app_desc ? app_desc->version : "unknown";

    const board_button_t *buttons = NULL;
    size_t button_count = board_buttons(&buttons);
    char uuid_field[sizeof(",\"device_uuid\":\"\"") + sizeof(s_device_uuid) - 1] = "";
    if (s_device_uuid[0] != '\0') {
        snprintf(uuid_field, sizeof(uuid_field), ",\"device_uuid\":\"%s\"", s_device_uuid);
    }
    const char *interaction_modes = ",\"interaction_modes\":[\"hold_to_talk\",\"click_to_talk\"]";
    const char *ui_states = ",\"ui_states\":[\"ready\",\"recording\",\"thinking\","
                            "\"pending_confirmation\",\"error\"]";
    /* Keep device identity and button roles ahead of optional capabilities. */
    const size_t max_json = ble_att_mtu(s_conn_handle) - 3 - 4;
    char json[320];
    while (true) {
        char button_list[96] = "";
        size_t used = 0;
        for (size_t i = 0; i < button_count && used < sizeof(button_list); ++i) {
            used += snprintf(button_list + used, sizeof(button_list) - used, "%s\"%s\"",
                             i ? "," : "", buttons[i].name);
        }
        const int len = snprintf(json, sizeof(json),
                                 "{\"event\":\"device_info\",\"hardware\":\"%s\","
                                 "\"firmware_version\":\"%s\",\"buttons\":[%s]%s%s%s}",
                                 board_hardware_name(), version, button_list, uuid_field,
                                 interaction_modes, ui_states);
        if (len >= 0 && (size_t)len < sizeof(json) && (size_t)len <= max_json) {
            return send_state_json(json);
        }
        if (*ui_states) {
            ui_states = "";
        } else if (*interaction_modes) {
            interaction_modes = "";
        } else if (button_count > 0) {
            button_count--;
        } else {
            return ESP_ERR_INVALID_SIZE;
        }
    }
}

esp_err_t voice_ble_send_button_down(const char *button, uint32_t session_id)
{
    char json[96];
    if (session_id > 0) {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_down\",\"button\":\"%s\",\"session_id\":%" PRIu32 "}",
                 button, session_id);
    } else {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_down\",\"button\":\"%s\"}", button);
    }
    return send_state_json(json);
}

esp_err_t voice_ble_send_button_up(const char *button, uint32_t duration_ms,
                                   uint32_t session_id)
{
    char json[128];
    if (session_id > 0) {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_up\",\"button\":\"%s\","
                 "\"duration_ms\":%" PRIu32 ",\"session_id\":%" PRIu32 "}",
                 button, duration_ms, session_id);
    } else {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_up\",\"button\":\"%s\",\"duration_ms\":%" PRIu32 "}",
                 button, duration_ms);
    }
    return send_state_json(json);
}

esp_err_t voice_ble_send_button_click(const char *button, uint32_t duration_ms,
                                      uint32_t session_id)
{
    char json[128];
    if (session_id > 0) {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_click\",\"button\":\"%s\","
                 "\"duration_ms\":%" PRIu32 ",\"session_id\":%" PRIu32 "}",
                 button, duration_ms, session_id);
    } else if (duration_ms > 0) {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_click\",\"button\":\"%s\",\"duration_ms\":%" PRIu32 "}",
                 button, duration_ms);
    } else {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_click\",\"button\":\"%s\"}", button);
    }
    ESP_LOGI(TAG, "button click button=%s session=%" PRIu32 " duration_ms=%" PRIu32,
             button, session_id, duration_ms);
    return send_state_json(json);
}

esp_err_t voice_ble_send_confirmation_shake(const char *confirmation_id)
{
    char json[96];
    snprintf(json, sizeof(json), "{\"event\":\"confirmation_shake\",\"confirmation_id\":\"%s\"}",
             confirmation_id);
    return send_state_json(json);
}
