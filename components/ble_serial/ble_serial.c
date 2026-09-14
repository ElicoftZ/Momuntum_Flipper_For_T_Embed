/**
 * @file ble_serial.c
 * Flipper BLE serial, device-information, battery and current-time services.
 *
 * This implementation uses the ESP-IDF NimBLE host. UUIDs and wire formats
 * remain compatible with the official Flipper mobile applications.
 */

#include "ble_serial.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>

#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_sm.h>
#include <nimble/ble.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>

#include <datetime/datetime.h>
#include <furi_ble/gap.h>
#include <furi_hal_rtc.h>
#include <furi_hal_version.h>
#include <nimble_glue.h>
#include <protobuf_version.h>

#define TAG "ble_serial"

#define BLE_SERIAL_STR_HELPER(x) #x
#define BLE_SERIAL_STR(x) BLE_SERIAL_STR_HELPER(x)

#define CTS_VALUE_SIZE     10
#define CTS_MIN_WRITE_SIZE 7

extern void furi_hal_bt_emit_gap_event(GapEvent event);

static const ble_uuid128_t serial_svc_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xfe, 0x60, 0xcc, 0x7a, 0x48, 0x2a,
    0x98, 0x4a, 0x7f, 0x2e, 0xd5, 0xb3, 0xe5, 0x8f);
static const ble_uuid128_t serial_tx_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xfe, 0x61, 0x8e, 0x22, 0x45, 0x41,
    0x9d, 0x4c, 0x21, 0xed, 0xae, 0x82, 0xed, 0x19);
static const ble_uuid128_t serial_rx_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xfe, 0x62, 0x8e, 0x22, 0x45, 0x41,
    0x9d, 0x4c, 0x21, 0xed, 0xae, 0x82, 0xed, 0x19);
static const ble_uuid128_t serial_flow_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xfe, 0x63, 0x8e, 0x22, 0x45, 0x41,
    0x9d, 0x4c, 0x21, 0xed, 0xae, 0x82, 0xed, 0x19);
static const ble_uuid128_t serial_rpc_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xfe, 0x64, 0x8e, 0x22, 0x45, 0x41,
    0x9d, 0x4c, 0x21, 0xed, 0xae, 0x82, 0xed, 0x19);
static const ble_uuid128_t dis_api_version_uuid = BLE_UUID128_INIT(
    0x33, 0xa9, 0xb5, 0x3e, 0x87, 0x5d, 0x1a, 0x8e,
    0xc8, 0x47, 0x5e, 0xae, 0x6d, 0x66, 0xf6, 0x03);

static const ble_uuid16_t cts_service_uuid = BLE_UUID16_INIT(0x1805);
static const ble_uuid16_t cts_characteristic_uuid = BLE_UUID16_INIT(0x2a2b);

static const char dis_manufacturer[] = "Flipper Devices Inc.";
static const char dis_hw_revision[] = "ESP32-S3 1.0";
static const char dis_sw_revision[] = "1.4.3";
static const char dis_api_version[] =
    BLE_SERIAL_STR(PROTOBUF_MAJOR_VERSION) "." BLE_SERIAL_STR(PROTOBUF_MINOR_VERSION);
static const uint8_t bas_battery_level = 100;

typedef enum {
    SerialAttrTx = 1,
    SerialAttrRx,
    SerialAttrFlow,
    SerialAttrRpc,
    SerialAttrManufacturer,
    SerialAttrHardwareRevision,
    SerialAttrSoftwareRevision,
    SerialAttrApiVersion,
    SerialAttrBattery,
    SerialAttrCurrentTime,
} SerialAttribute;

struct BleSerial {
    SemaphoreHandle_t mutex;
    BleSerialConfig config;
    bool connected;
    uint16_t conn_handle;
    SerialServiceEventCallback event_callback;
    void* event_context;
    uint16_t buff_size;
    uint16_t bytes_ready;
    BleSerialStateCallback state_callback;
    void* state_context;
    uint32_t rpc_status;
};

static struct {
    SemaphoreHandle_t mutex;
    BleSerial* active;
    bool ready;
    bool advertising;
    bool advertising_requested;
    bool tx_indicate_enabled;
    bool flow_notify_enabled;
    bool rpc_notify_enabled;
    uint16_t tx_handle;
    uint16_t rx_handle;
    uint16_t flow_handle;
    uint16_t rpc_handle;
} serial_state;

static struct {
    bool busy;
    uint16_t conn_handle;
    uint16_t service_start;
    uint16_t service_end;
} cts_client;

static void serial_lock(BleSerial* serial) {
    if(serial && serial->mutex) xSemaphoreTake(serial->mutex, portMAX_DELAY);
}

static void serial_unlock(BleSerial* serial) {
    if(serial && serial->mutex) xSemaphoreGive(serial->mutex);
}

static void serial_lock_global(void) {
    if(serial_state.mutex) xSemaphoreTake(serial_state.mutex, portMAX_DELAY);
}

static void serial_unlock_global(void) {
    if(serial_state.mutex) xSemaphoreGive(serial_state.mutex);
}

static int append_value(struct os_mbuf* om, const void* data, size_t size) {
    return os_mbuf_append(om, data, size) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static void cts_fill_value(uint8_t value[CTS_VALUE_SIZE]) {
    DateTime datetime;
    furi_hal_rtc_get_datetime(&datetime);
    value[0] = datetime.year & 0xff;
    value[1] = datetime.year >> 8;
    value[2] = datetime.month;
    value[3] = datetime.day;
    value[4] = datetime.hour;
    value[5] = datetime.minute;
    value[6] = datetime.second;
    value[7] = datetime.weekday;
    value[8] = 0;
    value[9] = 0;
}

static bool cts_apply_write(const uint8_t* value, uint16_t length) {
    if(length < CTS_MIN_WRITE_SIZE) return false;
    DateTime datetime = {
        .year = (uint16_t)(value[0] | (value[1] << 8)),
        .month = value[2],
        .day = value[3],
        .hour = value[4],
        .minute = value[5],
        .second = value[6],
        .weekday = 1,
    };
    if(!datetime_validate_datetime(&datetime)) {
        ESP_LOGW(TAG, "Rejected invalid Current Time value");
        return false;
    }
    datetime_timestamp_to_datetime(datetime_datetime_to_timestamp(&datetime), &datetime);
    furi_hal_rtc_set_datetime(&datetime);
    ESP_LOGI(
        TAG,
        "Clock set from BLE: %04u-%02u-%02u %02u:%02u:%02u",
        datetime.year,
        datetime.month,
        datetime.day,
        datetime.hour,
        datetime.minute,
        datetime.second);
    return true;
}

static bool serial_send_value(uint16_t handle, const void* value, size_t size, bool indicate) {
    BleSerial* serial = serial_state.active;
    bool connected = serial && serial->connected;
    uint16_t conn_handle = connected ? serial->conn_handle : BLE_HS_CONN_HANDLE_NONE;
    if(!connected || !handle) return false;

    struct os_mbuf* om = ble_hs_mbuf_from_flat(value, size);
    if(!om) return false;
    int rc = indicate ? ble_gatts_indicate_custom(conn_handle, handle, om) :
                        ble_gatts_notify_custom(conn_handle, handle, om);
    return rc == 0;
}

static void serial_send_flow(BleSerial* serial) {
    if(!serial || !serial_state.flow_notify_enabled) return;
    uint32_t flow = __builtin_bswap32((uint32_t)serial->buff_size);
    serial_send_value(serial_state.flow_handle, &flow, sizeof(flow), false);
}

static void serial_send_rpc(BleSerial* serial) {
    if(!serial || !serial_state.rpc_notify_enabled) return;
    serial_send_value(
        serial_state.rpc_handle, &serial->rpc_status, sizeof(serial->rpc_status), false);
}

static int serial_access(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* ctxt,
    void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    SerialAttribute attribute = (SerialAttribute)(uintptr_t)arg;

    serial_lock_global();
    BleSerial* serial = serial_state.active;
    serial_unlock_global();
    if(!serial) return BLE_ATT_ERR_UNLIKELY;

    if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        switch(attribute) {
        case SerialAttrTx:
        case SerialAttrRx:
            return 0;
        case SerialAttrFlow: {
            uint32_t flow = __builtin_bswap32((uint32_t)serial->buff_size);
            return append_value(ctxt->om, &flow, sizeof(flow));
        }
        case SerialAttrRpc:
            return append_value(ctxt->om, &serial->rpc_status, sizeof(serial->rpc_status));
        case SerialAttrManufacturer:
            return append_value(ctxt->om, dis_manufacturer, sizeof(dis_manufacturer) - 1);
        case SerialAttrHardwareRevision:
            return append_value(ctxt->om, dis_hw_revision, sizeof(dis_hw_revision) - 1);
        case SerialAttrSoftwareRevision:
            return append_value(ctxt->om, dis_sw_revision, sizeof(dis_sw_revision) - 1);
        case SerialAttrApiVersion:
            return append_value(ctxt->om, dis_api_version, sizeof(dis_api_version) - 1);
        case SerialAttrBattery:
            return append_value(ctxt->om, &bas_battery_level, sizeof(bas_battery_level));
        case SerialAttrCurrentTime: {
            uint8_t value[CTS_VALUE_SIZE];
            cts_fill_value(value);
            return append_value(ctxt->om, value, sizeof(value));
        }
        default:
            return BLE_ATT_ERR_UNLIKELY;
        }
    }

    if(ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    size_t length = OS_MBUF_PKTLEN(ctxt->om);
    if(length > BLE_SVC_SERIAL_DATA_LEN_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t value[BLE_SVC_SERIAL_DATA_LEN_MAX];
    uint16_t copied = 0;
    if(ble_hs_mbuf_to_flat(ctxt->om, value, sizeof(value), &copied) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if(attribute == SerialAttrCurrentTime) {
        return cts_apply_write(value, copied) ? 0 : BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    if(attribute == SerialAttrRx) {
        SerialServiceEventCallback callback;
        void* context;
        serial_lock(serial);
        callback = serial->event_callback;
        context = serial->event_context;
        serial->bytes_ready = serial->bytes_ready >= copied ? serial->bytes_ready - copied : 0;
        serial_unlock(serial);

        if(callback) {
            SerialServiceEvent event = {
                .event = SerialServiceEventTypeDataReceived,
                .data = {.buffer = value, .size = copied},
            };
            /* Credit is replenished only after the advertised window drains.
             * Replacing it with stream free space stalls long RPC uploads. */
            (void)callback(event, context);
        }
        return 0;
    }
    if(attribute == SerialAttrRpc) {
        if(copied < sizeof(uint32_t)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        memcpy(&serial->rpc_status, value, sizeof(serial->rpc_status));
        if(serial->rpc_status == FuriHalBtSerialRpcStatusNotActive) {
            serial_lock(serial);
            SerialServiceEventCallback callback = serial->event_callback;
            void* context = serial->event_context;
            serial_unlock(serial);
            if(callback) {
                SerialServiceEvent event = {.event = SerialServiceEventTypesBleResetRequest};
                callback(event, context);
            }
        }
        return 0;
    }
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
}

#define SERIAL_ARG(value) ((void*)(uintptr_t)(value))

static const struct ble_gatt_svc_def serial_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &serial_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = &serial_tx_uuid.u,
             .access_cb = serial_access,
             .arg = SERIAL_ARG(SerialAttrTx),
             .val_handle = &serial_state.tx_handle,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_INDICATE},
            {.uuid = &serial_rx_uuid.u,
             .access_cb = serial_access,
             .arg = SERIAL_ARG(SerialAttrRx),
             .val_handle = &serial_state.rx_handle,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_WRITE_NO_RSP},
            {.uuid = &serial_flow_uuid.u,
             .access_cb = serial_access,
             .arg = SERIAL_ARG(SerialAttrFlow),
             .val_handle = &serial_state.flow_handle,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY},
            {.uuid = &serial_rpc_uuid.u,
             .access_cb = serial_access,
             .arg = SERIAL_ARG(SerialAttrRpc),
             .val_handle = &serial_state.rpc_handle,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_NOTIFY},
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180a),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = BLE_UUID16_DECLARE(0x2a29),
             .access_cb = serial_access,
             .arg = SERIAL_ARG(SerialAttrManufacturer),
             .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a27),
             .access_cb = serial_access,
             .arg = SERIAL_ARG(SerialAttrHardwareRevision),
             .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a28),
             .access_cb = serial_access,
             .arg = SERIAL_ARG(SerialAttrSoftwareRevision),
             .flags = BLE_GATT_CHR_F_READ},
            {.uuid = &dis_api_version_uuid.u,
             .access_cb = serial_access,
             .arg = SERIAL_ARG(SerialAttrApiVersion),
             .flags = BLE_GATT_CHR_F_READ},
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180f),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = BLE_UUID16_DECLARE(0x2a19),
             .access_cb = serial_access,
             .arg = SERIAL_ARG(SerialAttrBattery),
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY},
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &cts_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = &cts_characteristic_uuid.u,
             .access_cb = serial_access,
             .arg = SERIAL_ARG(SerialAttrCurrentTime),
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_WRITE_NO_RSP},
            {0},
        },
    },
    {0},
};

static void serial_update_connection(BleSerial* serial, bool connected) {
    serial_lock(serial);
    serial->connected = connected;
    BleSerialStateCallback callback = serial->state_callback;
    void* context = serial->state_context;
    serial_unlock(serial);
    if(callback) callback(connected, context);
}

static void cts_client_finish(const char* reason) {
    cts_client.busy = false;
    cts_client.service_start = 0;
    cts_client.service_end = 0;
    if(reason) ESP_LOGI(TAG, "CTS client: %s", reason);
}

static int cts_client_read_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    struct ble_gatt_attr* attr,
    void* arg) {
    (void)conn_handle;
    (void)arg;
    if(error->status == 0 && attr && attr->om) {
        uint8_t value[CTS_VALUE_SIZE];
        uint16_t length = 0;
        if(ble_hs_mbuf_to_flat(attr->om, value, sizeof(value), &length) == 0) {
            cts_apply_write(value, length);
            cts_client_finish("clock read from peer");
            return 0;
        }
    }
    cts_client_finish("peer time read failed");
    return 0;
}

static int cts_client_chr_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    const struct ble_gatt_chr* chr,
    void* arg) {
    (void)arg;
    if(error->status == 0 && chr) {
        ble_gattc_read(conn_handle, chr->val_handle, cts_client_read_cb, NULL);
        return 0;
    }
    if(error->status == BLE_HS_EDONE) cts_client_finish("peer has no Current Time value");
    return 0;
}

static int cts_client_service_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    const struct ble_gatt_svc* service,
    void* arg) {
    (void)arg;
    if(error->status == 0 && service) {
        cts_client.service_start = service->start_handle;
        cts_client.service_end = service->end_handle;
        return 0;
    }
    if(error->status == BLE_HS_EDONE && cts_client.service_start) {
        int rc = ble_gattc_disc_chrs_by_uuid(
            conn_handle,
            cts_client.service_start,
            cts_client.service_end,
            &cts_characteristic_uuid.u,
            cts_client_chr_cb,
            NULL);
        if(rc != 0) cts_client_finish("Current Time characteristic discovery failed");
    } else if(error->status != 0 || !cts_client.service_start) {
        cts_client_finish("peer publishes no Current Time Service");
    }
    return 0;
}

static void cts_client_start(uint16_t conn_handle) {
    if(cts_client.busy) return;
    cts_client.busy = true;
    cts_client.conn_handle = conn_handle;
    cts_client.service_start = 0;
    cts_client.service_end = 0;
    int rc = ble_gattc_disc_svc_by_uuid(
        conn_handle, &cts_service_uuid.u, cts_client_service_cb, NULL);
    if(rc != 0) cts_client_finish("service discovery could not start");
}

static bool serial_try_start_advertising_locked(void);

/** Ask the phone for connection parameters that survive ESP32 Wi-Fi/BLE
 *  coexistence. The Flipper mobile app is much happier when the peripheral
 *  drives this instead of accepting whatever the phone picked: the stock
 *  Android default is often a long interval with a short supervision timeout,
 *  which the coexisting radio cannot always honour. Values follow the
 *  Flipper serial profile: 7.5 ms .. 45 ms, no latency, 4 s supervision. */
static void serial_request_conn_params(uint16_t conn_handle) {
    struct ble_gap_upd_params params = {
        .itvl_min = 0x06, /* 7.5 ms */
        .itvl_max = 0x24, /* 45 ms */
        .latency = 0,
        .supervision_timeout = 0x0190, /* 400 * 10 ms = 4 s */
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    int rc = ble_gap_update_params(conn_handle, &params);
    if(rc != 0) {
        ESP_LOGW(TAG, "Connection parameter update request failed, rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Requested connection parameters (7.5-45 ms, 4 s timeout)");
    }
}

static int serial_gap_event(struct ble_gap_event* event, void* arg) {
    (void)arg;
    /* NimBLE reports notification sends synchronously, including sends made
     * below while serial_state.mutex is held. Only indications need handling. */
    if(event->type == BLE_GAP_EVENT_NOTIFY_TX && !event->notify_tx.indication) return 0;

    serial_lock_global();
    BleSerial* serial = serial_state.active;

    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        serial_state.advertising = false;
        if(event->connect.status == 0 && serial) {
            serial->conn_handle = event->connect.conn_handle;
            serial_state.tx_indicate_enabled = false;
            serial_state.flow_notify_enabled = false;
            serial_state.rpc_notify_enabled = false;
            serial_unlock_global();
            serial_request_conn_params(event->connect.conn_handle);
            serial_update_connection(serial, true);
            int rc = ble_gap_security_initiate(event->connect.conn_handle);
            ESP_LOGI(TAG, "Security initiation: rc=%d", rc);
            return 0;
        }
        serial_try_start_advertising_locked();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected, reason=%d (0x%x)",
                 event->disconnect.reason, event->disconnect.reason);
        cts_client_finish(NULL);
        if(serial) {
            serial->conn_handle = BLE_HS_CONN_HANDLE_NONE;
            serial_unlock_global();
            serial_update_connection(serial, false);
            serial_lock_global();
        }
        serial_state.advertising = false;
        serial_state.tx_indicate_enabled = false;
        serial_state.flow_notify_enabled = false;
        serial_state.rpc_notify_enabled = false;
        serial_try_start_advertising_locked();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        serial_state.advertising = false;
        serial_try_start_advertising_locked();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if(event->subscribe.attr_handle == serial_state.tx_handle) {
            serial_state.tx_indicate_enabled = event->subscribe.cur_indicate;
        } else if(event->subscribe.attr_handle == serial_state.flow_handle) {
            serial_state.flow_notify_enabled = event->subscribe.cur_notify;
            if(serial_state.flow_notify_enabled && serial) serial_send_flow(serial);
        } else if(event->subscribe.attr_handle == serial_state.rpc_handle) {
            serial_state.rpc_notify_enabled = event->subscribe.cur_notify;
            if(serial_state.rpc_notify_enabled && serial) serial_send_rpc(serial);
        }
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if(serial && event->notify_tx.attr_handle == serial_state.tx_handle &&
           event->notify_tx.indication && event->notify_tx.status == 0) {
            serial_lock(serial);
            SerialServiceEventCallback callback = serial->event_callback;
            void* context = serial->event_context;
            serial_unlock(serial);
            if(callback) {
                SerialServiceEvent sent = {.event = SerialServiceEventTypeDataSent};
                callback(sent, context);
            }
        }
        break;
    case BLE_GAP_EVENT_MTU:
        if(serial && serial->connected && event->mtu.value > 3) {
            GapEvent mtu = {
                .type = GapEventTypeUpdateMTU,
                .data.max_packet_size = event->mtu.value - 3,
            };
            furi_hal_bt_emit_gap_event(mtu);
        }
        break;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if(event->conn_update.status == 0 &&
           ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(
                TAG,
                "Connection parameters: itvl=%u (%.1f ms) latency=%u timeout=%u (%.0f ms)",
                desc.conn_itvl,
                desc.conn_itvl * 1.25,
                desc.conn_latency,
                desc.supervision_timeout,
                desc.supervision_timeout * 10.0);
        } else {
            ESP_LOGW(TAG, "Connection parameter update failed, status=%d", event->conn_update.status);
        }
        break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
        if(event->enc_change.status == 0 && serial) {
            ESP_LOGI(TAG, "Encryption established");
            serial_send_flow(serial);
            serial_send_rpc(serial);
            cts_client_start(event->enc_change.conn_handle);
        } else if(event->enc_change.status != 0) {
            ESP_LOGE(
                TAG,
                "Encryption/pairing failed, status=%d (0x%x)",
                event->enc_change.status,
                event->enc_change.status);
        }
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io = {.action = event->passkey.params.action};
        /* UI callbacks and SM injection can call back into GAP. */
        serial_unlock_global();
        if(io.action == BLE_SM_IOACT_DISP) {
            io.passkey = esp_random() % 1000000U;
            ESP_LOGI(TAG, "Pairing passkey: %06lu", (unsigned long)io.passkey);
            GapEvent pin = {.type = GapEventTypePinCodeShow, .data.pin_code = io.passkey};
            furi_hal_bt_emit_gap_event(pin);
        } else if(io.action == BLE_SM_IOACT_NUMCMP) {
            ESP_LOGI(TAG, "Pairing numeric comparison: %06lu",
                     (unsigned long)event->passkey.params.numcmp);
            GapEvent pin = {
                .type = GapEventTypePinCodeShow,
                .data.pin_code = event->passkey.params.numcmp,
            };
            furi_hal_bt_emit_gap_event(pin);
            io.numcmp_accept = 1;
        } else {
            /* Neither advertised IO capability has a keyboard. In particular,
             * INPUT must never be answered with a locally generated passkey. */
            ESP_LOGE(TAG, "Unsupported pairing action: %d; disconnecting", io.action);
            int rc = ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
            ESP_LOGI(TAG, "Pairing rejection: rc=%d", rc);
            return 0;
        }
        int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
        ESP_LOGI(TAG, "Pairing IO injection: action=%d rc=%d", io.action, rc);
        return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if(ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        serial_unlock_global();
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }

    serial_unlock_global();
    return 0;
}

static bool serial_append_adv_field(
    uint8_t* data,
    size_t* offset,
    uint8_t type,
    const void* value,
    size_t value_len) {
    if(*offset + value_len + 2 > BLE_HS_ADV_MAX_SZ) return false;
    data[*offset] = value_len + 1;
    data[*offset + 1] = type;
    memcpy(data + *offset + 2, value, value_len);
    *offset += value_len + 2;
    return true;
}

static int serial_configure_advertising(const char* name) {
    uint8_t data[BLE_HS_ADV_MAX_SZ] = {0};
    size_t offset = 0;
    const uint8_t flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    uint16_t uuid = 0x3080;
    FuriHalVersionColor color = furi_hal_version_get_hw_color();
    if(color == FuriHalVersionColorBlack) uuid |= 1;
    else if(color == FuriHalVersionColorWhite) uuid |= 2;
    else if(color == FuriHalVersionColorTransparent) uuid |= 3;
    const uint8_t service_uuid[] = {(uint8_t)uuid, (uint8_t)(uuid >> 8)};
    serial_append_adv_field(data, &offset, 0x01, &flags, sizeof(flags));
    serial_append_adv_field(data, &offset, 0x03, service_uuid, sizeof(service_uuid));
    size_t full_name_len = strlen(name);
    size_t name_len = full_name_len;
    while(name_len && offset + name_len + 2 > sizeof(data)) name_len--;
    if(name_len) {
        serial_append_adv_field(
            data, &offset, name_len == full_name_len ? 0x09 : 0x08, name, name_len);
    }
    return ble_gap_adv_set_data(data, (int)offset);
}

static bool serial_try_start_advertising_locked(void) {
    BleSerial* serial = serial_state.active;
    if(!serial || !serial_state.ready || !serial_state.advertising_requested ||
       serial_state.advertising || serial->connected) {
        return false;
    }
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 0x20;
    params.itvl_max = 0x40;
    int rc = ble_gap_adv_start(
        nimble_glue_own_address_type(),
        NULL,
        BLE_HS_FOREVER,
        &params,
        serial_gap_event,
        NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed, rc=%d", rc);
        return false;
    }
    serial_state.advertising = true;
    return true;
}

static void serial_on_sync(void* context) {
    (void)context;
    serial_lock_global();
    serial_state.ready = true;
    serial_try_start_advertising_locked();
    serial_unlock_global();
}

static bool serial_has_custom_mac(const BleSerialConfig* config) {
    static const uint8_t zero[6] = {0};
    return memcmp(config->mac, zero, sizeof(zero)) != 0;
}

void ble_serial_get_default_mac(uint8_t mac[6]) {
    if(esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) memset(mac, 0, 6);
}

BleSerial* ble_serial_alloc(const BleSerialConfig* config) {
    if(!config) return NULL;
    if(!serial_state.mutex) {
        serial_state.mutex = xSemaphoreCreateMutex();
        if(!serial_state.mutex) return NULL;
    }
    serial_lock_global();
    if(serial_state.active) {
        serial_unlock_global();
        return NULL;
    }
    serial_unlock_global();

    BleSerial* serial = calloc(1, sizeof(BleSerial));
    if(!serial) return NULL;
    serial->mutex = xSemaphoreCreateMutex();
    if(!serial->mutex) {
        free(serial);
        return NULL;
    }
    memcpy(&serial->config, config, sizeof(*config));
    serial->conn_handle = BLE_HS_CONN_HANDLE_NONE;

    serial_lock_global();
    serial_state.active = serial;
    serial_state.ready = false;
    serial_state.advertising = false;
    serial_state.advertising_requested = false;
    serial_state.tx_indicate_enabled = false;
    serial_state.flow_notify_enabled = false;
    serial_state.rpc_notify_enabled = false;
    serial_unlock_global();

    esp_err_t err = nimble_glue_init(config->device_name);
    if(err != ESP_OK) goto error;
    uint8_t io_cap = config->pairing == BleSerialPairingPinCodeVerifyYesNo ?
                         BLE_HS_IO_DISPLAY_YESNO :
                         BLE_HS_IO_DISPLAY_ONLY;
    nimble_glue_configure_security(config->bonding, true, true, io_cap);
    if(serial_has_custom_mac(config)) {
        err = nimble_glue_set_random_address(config->mac);
        if(err != ESP_OK) goto error;
    }

    int rc = ble_gatts_count_cfg(serial_services);
    if(rc == 0) rc = ble_gatts_add_svcs(serial_services);
    if(rc != 0) {
        ESP_LOGE(TAG, "Failed to register serial services, rc=%d", rc);
        goto error;
    }
    ble_att_set_preferred_mtu(BLE_SVC_SERIAL_DATA_LEN_MAX + 3);
    err = nimble_glue_start(serial_on_sync, NULL);
    if(err != ESP_OK) goto error;
    rc = serial_configure_advertising(config->device_name);
    if(rc != 0) {
        ESP_LOGE(TAG, "Failed to configure serial advertising, rc=%d", rc);
        goto error;
    }
    ESP_LOGI(TAG, "NimBLE serial ready: %s", config->device_name);
    return serial;

error:
    nimble_glue_stop();
    serial_lock_global();
    if(serial_state.active == serial) serial_state.active = NULL;
    serial_state.ready = false;
    serial_unlock_global();
    vSemaphoreDelete(serial->mutex);
    free(serial);
    return NULL;
}

void ble_serial_free(BleSerial* serial) {
    if(!serial) return;
    ble_serial_stop_advertising();
    if(serial->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(serial->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    nimble_glue_stop();
    serial_lock_global();
    if(serial_state.active == serial) serial_state.active = NULL;
    serial_state.ready = false;
    serial_unlock_global();
    vSemaphoreDelete(serial->mutex);
    free(serial);
}

void ble_serial_reset_initialized(void) {
    if(!serial_state.mutex) return;
    serial_lock_global();
    serial_state.ready = false;
    serial_state.advertising = false;
    serial_state.advertising_requested = false;
    serial_state.active = NULL;
    serial_unlock_global();
}

void ble_serial_set_state_callback(
    BleSerial* serial,
    BleSerialStateCallback callback,
    void* context) {
    if(!serial) return;
    serial_lock(serial);
    serial->state_callback = callback;
    serial->state_context = context;
    bool connected = serial->connected;
    serial_unlock(serial);
    if(callback) callback(connected, context);
}

bool ble_serial_is_connected(BleSerial* serial) {
    if(!serial) return false;
    serial_lock(serial);
    bool connected = serial->connected;
    serial_unlock(serial);
    return connected;
}

void ble_serial_set_event_callback(
    BleSerial* serial,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context) {
    if(!serial) return;
    serial_lock(serial);
    serial->event_callback = callback;
    serial->event_context = context;
    serial->buff_size = buff_size;
    serial->bytes_ready = buff_size;
    bool connected = serial->connected;
    serial_unlock(serial);
    if(connected) serial_send_flow(serial);
}

bool ble_serial_tx(BleSerial* serial, uint8_t* data, uint16_t size) {
    if(!serial || !data || !size || size > BLE_SVC_SERIAL_DATA_LEN_MAX ||
       !serial_state.tx_indicate_enabled) {
        return false;
    }
    return serial_send_value(serial_state.tx_handle, data, size, true);
}

void ble_serial_set_rpc_active(BleSerial* serial, FuriHalBtSerialRpcStatus status) {
    if(!serial) return;
    serial->rpc_status = (uint32_t)status;
    if(serial->connected) serial_send_rpc(serial);
}

void ble_serial_notify_buffer_is_empty(BleSerial* serial) {
    if(!serial) return;
    serial_lock(serial);
    bool was_empty = serial->bytes_ready == 0;
    if(was_empty) serial->bytes_ready = serial->buff_size;
    bool connected = serial->connected;
    serial_unlock(serial);
    if(was_empty && connected) serial_send_flow(serial);
}

bool ble_serial_start_advertising(void) {
    serial_lock_global();
    bool accepted = serial_state.active != NULL;
    if(accepted) {
        serial_state.advertising_requested = true;
        serial_try_start_advertising_locked();
    }
    serial_unlock_global();
    return accepted;
}

void ble_serial_stop_advertising(void) {
    serial_lock_global();
    serial_state.advertising_requested = false;
    if(serial_state.advertising) {
        ble_gap_adv_stop();
        serial_state.advertising = false;
    }
    serial_unlock_global();
}

bool ble_serial_is_advertising(void) {
    if(!serial_state.mutex) return false;
    serial_lock_global();
    bool advertising = serial_state.advertising_requested && serial_state.active &&
                       !serial_state.active->connected;
    serial_unlock_global();
    return advertising;
}

bool ble_serial_is_active(void) {
    if(!serial_state.mutex) return false;
    serial_lock_global();
    bool active = serial_state.active != NULL;
    serial_unlock_global();
    return active;
}

void ble_serial_refresh_advertising(void) {
    if(!serial_state.mutex) return;
    serial_lock_global();
    BleSerial* serial = serial_state.active;
    if(!serial || !serial_state.ready) {
        serial_unlock_global();
        return;
    }
    if(serial_state.advertising) {
        const int rc = ble_gap_adv_stop();
        if(rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "Cannot pause advertising for refresh, rc=%d", rc);
            serial_unlock_global();
            return;
        }
        serial_state.advertising = false;
    }
    const int rc = serial_configure_advertising(serial->config.device_name);
    if(rc != 0) ESP_LOGE(TAG, "Cannot refresh advertising, rc=%d", rc);
    serial_try_start_advertising_locked();
    serial_unlock_global();
}

bool ble_serial_set_device_name(BleSerial* serial, const char* device_name) {
    if(!serial || !device_name || !device_name[0] ||
       strnlen(device_name, BLE_SERIAL_DEVICE_NAME_LEN + 1U) > BLE_SERIAL_DEVICE_NAME_LEN) {
        return false;
    }

    serial_lock_global();
    if(serial_state.active != serial || !serial_state.ready) {
        serial_unlock_global();
        return false;
    }

    const bool restart_advertising =
        serial_state.advertising_requested && serial_state.advertising && !serial->connected;
    if(serial_state.advertising) {
        const int stop_rc = ble_gap_adv_stop();
        if(stop_rc != 0 && stop_rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "Cannot pause advertising for name update, rc=%d", stop_rc);
            serial_unlock_global();
            return false;
        }
        serial_state.advertising = false;
    }

    int rc = ble_svc_gap_device_name_set(device_name);
    if(rc == 0) rc = serial_configure_advertising(device_name);
    if(rc == 0) {
        strlcpy(serial->config.device_name, device_name, sizeof(serial->config.device_name));
    } else {
        ESP_LOGE(TAG, "Cannot update BLE device name, rc=%d", rc);
    }

    bool restarted = true;
    if(restart_advertising) {
        restarted = serial_try_start_advertising_locked();
    }
    serial_unlock_global();
    return rc == 0 && restarted;
}

bool ble_serial_remove_pairing(void) {
    return nimble_glue_remove_all_bonds();
}
