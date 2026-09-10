#include "ble_hid.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_mac.h>

#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_sm.h>
#include <nimble/ble.h>
#include <os/os_mbuf.h>

#include <nimble_glue.h>

#define TAG "ble_hid"

#define BLE_HID_REPORT_ID_KEYBOARD 1
#define BLE_HID_REPORT_ID_MOUSE    2
#define BLE_HID_REPORT_ID_CONSUMER 3
#define BLE_HID_SERVICE_UUID       0x1812
#define BLE_HID_APPEARANCE_KEYBOARD 0x03c1
#define BLE_HID_KEYBOARD_KEYS_MAX  6

typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[BLE_HID_KEYBOARD_KEYS_MAX];
} __attribute__((packed)) BleHidKeyboardReport;

typedef struct {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
} __attribute__((packed)) BleHidMouseReport;

typedef struct {
    uint16_t key;
} __attribute__((packed)) BleHidConsumerReport;

struct BleHid {
    BleHidConfig config;
    SemaphoreHandle_t mutex;
    BleHidStateCallback state_callback;
    void* state_context;
    BleHidKeyboardReport keyboard_report;
    BleHidMouseReport mouse_report;
    BleHidConsumerReport consumer_report;
    uint8_t led_state;
    uint8_t protocol_mode;
    uint8_t control_point;
    bool connected;
    uint16_t conn_handle;
};

typedef enum {
    HidAttrInformation = 1,
    HidAttrReportMap,
    HidAttrControlPoint,
    HidAttrProtocolMode,
    HidAttrKeyboardInput,
    HidAttrKeyboardOutput,
    HidAttrMouseInput,
    HidAttrConsumerInput,
    HidAttrBootKeyboardInput,
    HidAttrBootKeyboardOutput,
    HidAttrBootMouseInput,
    HidAttrKeyboardInputRef,
    HidAttrKeyboardOutputRef,
    HidAttrMouseInputRef,
    HidAttrConsumerInputRef,
    HidAttrManufacturer,
    HidAttrPnpId,
    HidAttrBatteryLevel,
} HidAttribute;

static struct {
    SemaphoreHandle_t mutex;
    BleHid* active;
    bool advertising_requested;
    bool advertising;
    bool ready;
    uint16_t keyboard_input_handle;
    uint16_t mouse_input_handle;
    uint16_t consumer_input_handle;
    uint16_t boot_keyboard_input_handle;
    uint16_t boot_mouse_input_handle;
} ble_hid_state;

static const uint8_t ble_hid_report_map[] = {
    0x05, 0x01,
    0x09, 0x06,
    0xA1, 0x01,
    0x85, BLE_HID_REPORT_ID_KEYBOARD,
    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x01,
    0x05, 0x08,
    0x95, 0x08,
    0x75, 0x01,
    0x19, 0x01,
    0x29, 0x08,
    0x91, 0x02,
    0x95, BLE_HID_KEYBOARD_KEYS_MAX,
    0x75, 0x08,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x05, 0x07,
    0x19, 0x00,
    0x2A, 0xFF, 0x00,
    0x81, 0x00,
    0xC0,
    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
    0x09, 0x01,
    0xA1, 0x00,
    0x85, BLE_HID_REPORT_ID_MOUSE,
    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x03,
    0x75, 0x01,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x05,
    0x81, 0x03,
    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x09, 0x38,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x03,
    0x81, 0x06,
    0xC0,
    0xC0,
    0x05, 0x0C,
    0x09, 0x01,
    0xA1, 0x01,
    0x85, BLE_HID_REPORT_ID_CONSUMER,
    0x15, 0x00,
    0x26, 0xFF, 0x03,
    0x19, 0x00,
    0x2A, 0xFF, 0x03,
    0x95, 0x01,
    0x75, 0x10,
    0x81, 0x00,
    0xC0,
};

static const uint8_t hid_information[] = {0x11, 0x01, 0x00, 0x02};
static const char hid_manufacturer[] = "Flipper Devices";
static const uint8_t hid_pnp_id[] = {0x02, 0xc0, 0x16, 0xdf, 0x05, 0x00, 0x01};
static const uint8_t hid_battery_level = 100;

static void ble_hid_lock_global(void) {
    if(ble_hid_state.mutex) xSemaphoreTake(ble_hid_state.mutex, portMAX_DELAY);
}

static void ble_hid_unlock_global(void) {
    if(ble_hid_state.mutex) xSemaphoreGive(ble_hid_state.mutex);
}

static void ble_hid_lock(BleHid* hid) {
    if(hid && hid->mutex) xSemaphoreTake(hid->mutex, portMAX_DELAY);
}

static void ble_hid_unlock(BleHid* hid) {
    if(hid && hid->mutex) xSemaphoreGive(hid->mutex);
}

static int append_value(struct os_mbuf* om, const void* data, size_t size) {
    return os_mbuf_append(om, data, size) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int read_or_write_u8(
    struct ble_gatt_access_ctxt* ctxt,
    uint8_t* value,
    bool writable) {
    if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return append_value(ctxt->om, value, sizeof(*value));
    }
    if(writable && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if(OS_MBUF_PKTLEN(ctxt->om) != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        return ble_hs_mbuf_to_flat(ctxt->om, value, 1, NULL) == 0 ? 0 :
                                                                    BLE_ATT_ERR_UNLIKELY;
    }
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
}

static int ble_hid_access(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* ctxt,
    void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    HidAttribute attribute = (HidAttribute)(uintptr_t)arg;

    ble_hid_lock_global();
    BleHid* hid = ble_hid_state.active;
    ble_hid_unlock_global();
    if(!hid) return BLE_ATT_ERR_UNLIKELY;

    switch(attribute) {
    case HidAttrInformation:
        return append_value(ctxt->om, hid_information, sizeof(hid_information));
    case HidAttrReportMap:
        return append_value(ctxt->om, ble_hid_report_map, sizeof(ble_hid_report_map));
    case HidAttrControlPoint:
        return read_or_write_u8(ctxt, &hid->control_point, true);
    case HidAttrProtocolMode:
        return read_or_write_u8(ctxt, &hid->protocol_mode, true);
    case HidAttrKeyboardInput:
    case HidAttrBootKeyboardInput:
        return append_value(ctxt->om, &hid->keyboard_report, sizeof(hid->keyboard_report));
    case HidAttrMouseInput:
    case HidAttrBootMouseInput:
        return append_value(ctxt->om, &hid->mouse_report, sizeof(hid->mouse_report));
    case HidAttrConsumerInput:
        return append_value(ctxt->om, &hid->consumer_report, sizeof(hid->consumer_report));
    case HidAttrKeyboardOutput:
    case HidAttrBootKeyboardOutput:
        return read_or_write_u8(ctxt, &hid->led_state, true);
    case HidAttrKeyboardInputRef: {
        const uint8_t ref[] = {BLE_HID_REPORT_ID_KEYBOARD, 0x01};
        return append_value(ctxt->om, ref, sizeof(ref));
    }
    case HidAttrKeyboardOutputRef: {
        const uint8_t ref[] = {BLE_HID_REPORT_ID_KEYBOARD, 0x02};
        return append_value(ctxt->om, ref, sizeof(ref));
    }
    case HidAttrMouseInputRef: {
        const uint8_t ref[] = {BLE_HID_REPORT_ID_MOUSE, 0x01};
        return append_value(ctxt->om, ref, sizeof(ref));
    }
    case HidAttrConsumerInputRef: {
        const uint8_t ref[] = {BLE_HID_REPORT_ID_CONSUMER, 0x01};
        return append_value(ctxt->om, ref, sizeof(ref));
    }
    case HidAttrManufacturer:
        return append_value(ctxt->om, hid_manufacturer, sizeof(hid_manufacturer) - 1);
    case HidAttrPnpId:
        return append_value(ctxt->om, hid_pnp_id, sizeof(hid_pnp_id));
    case HidAttrBatteryLevel:
        return append_value(ctxt->om, &hid_battery_level, sizeof(hid_battery_level));
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

#define HID_ARG(value) ((void*)(uintptr_t)(value))
#define HID_REPORT_REF(value) \
    (struct ble_gatt_dsc_def[]) { \
        {.uuid = BLE_UUID16_DECLARE(0x2908), \
         .att_flags = BLE_ATT_F_READ, \
         .access_cb = ble_hid_access, \
         .arg = HID_ARG(value)}, \
        {0}, \
    }

static const struct ble_gatt_svc_def ble_hid_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_HID_SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = BLE_UUID16_DECLARE(0x2a4a),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrInformation),
             .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a4b),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrReportMap),
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = BLE_UUID16_DECLARE(0x2a4c),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrControlPoint),
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC},
            {.uuid = BLE_UUID16_DECLARE(0x2a4e),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrProtocolMode),
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP |
                      BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC},
            {.uuid = BLE_UUID16_DECLARE(0x2a4d),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrKeyboardInput),
             .descriptors = HID_REPORT_REF(HidAttrKeyboardInputRef),
             .val_handle = &ble_hid_state.keyboard_input_handle,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = BLE_UUID16_DECLARE(0x2a4d),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrKeyboardOutput),
             .descriptors = HID_REPORT_REF(HidAttrKeyboardOutputRef),
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_READ_ENC |
                      BLE_GATT_CHR_F_WRITE_ENC},
            {.uuid = BLE_UUID16_DECLARE(0x2a4d),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrMouseInput),
             .descriptors = HID_REPORT_REF(HidAttrMouseInputRef),
             .val_handle = &ble_hid_state.mouse_input_handle,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = BLE_UUID16_DECLARE(0x2a4d),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrConsumerInput),
             .descriptors = HID_REPORT_REF(HidAttrConsumerInputRef),
             .val_handle = &ble_hid_state.consumer_input_handle,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = BLE_UUID16_DECLARE(0x2a22),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrBootKeyboardInput),
             .val_handle = &ble_hid_state.boot_keyboard_input_handle,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = BLE_UUID16_DECLARE(0x2a32),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrBootKeyboardOutput),
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_READ_ENC |
                      BLE_GATT_CHR_F_WRITE_ENC},
            {.uuid = BLE_UUID16_DECLARE(0x2a33),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrBootMouseInput),
             .val_handle = &ble_hid_state.boot_mouse_input_handle,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC},
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180a),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = BLE_UUID16_DECLARE(0x2a29),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrManufacturer),
             .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a50),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrPnpId),
             .flags = BLE_GATT_CHR_F_READ},
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180f),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = BLE_UUID16_DECLARE(0x2a19),
             .access_cb = ble_hid_access,
             .arg = HID_ARG(HidAttrBatteryLevel),
             .flags = BLE_GATT_CHR_F_READ},
            {0},
        },
    },
    {0},
};

static void ble_hid_update_connection(BleHid* hid, bool connected) {
    ble_hid_lock(hid);
    hid->connected = connected;
    BleHidStateCallback callback = hid->state_callback;
    void* context = hid->state_context;
    ble_hid_unlock(hid);
    if(callback) callback(connected, context);
}

static bool ble_hid_append_adv_field(
    uint8_t* data,
    size_t* offset,
    uint8_t type,
    const void* value,
    size_t value_len) {
    if(*offset + value_len + 2 > BLE_HS_ADV_MAX_SZ) return false;
    data[*offset] = (uint8_t)value_len + 1;
    data[*offset + 1] = type;
    memcpy(data + *offset + 2, value, value_len);
    *offset += value_len + 2;
    return true;
}

static int ble_hid_configure_advertising(const char* name) {
    uint8_t data[BLE_HS_ADV_MAX_SZ] = {0};
    size_t offset = 0;
    const uint8_t flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const uint8_t uuid[] = {BLE_HID_SERVICE_UUID & 0xff, BLE_HID_SERVICE_UUID >> 8};
    const uint8_t appearance[] = {
        BLE_HID_APPEARANCE_KEYBOARD & 0xff,
        BLE_HID_APPEARANCE_KEYBOARD >> 8,
    };
    ble_hid_append_adv_field(data, &offset, 0x01, &flags, sizeof(flags));
    ble_hid_append_adv_field(data, &offset, 0x03, uuid, sizeof(uuid));
    ble_hid_append_adv_field(data, &offset, 0x19, appearance, sizeof(appearance));

    size_t full_len = strlen(name);
    size_t name_len = full_len;
    while(name_len > 0 && offset + name_len + 2 > sizeof(data)) name_len--;
    if(name_len > 0) {
        ble_hid_append_adv_field(
            data, &offset, name_len == full_len ? 0x09 : 0x08, name, name_len);
    }
    return ble_gap_adv_set_data(data, (int)offset);
}

static bool ble_hid_try_start_advertising_locked(void);

static int ble_hid_gap_event(struct ble_gap_event* event, void* arg) {
    (void)arg;
    ble_hid_lock_global();
    BleHid* hid = ble_hid_state.active;

    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ble_hid_state.advertising = false;
        if(event->connect.status == 0 && hid) {
            hid->conn_handle = event->connect.conn_handle;
            ble_hid_unlock_global();
            ble_hid_update_connection(hid, true);
            ble_gap_security_initiate(event->connect.conn_handle);
            return 0;
        }
        ble_hid_try_start_advertising_locked();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        if(hid) {
            hid->conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_hid_unlock_global();
            ble_hid_update_connection(hid, false);
            ble_hid_lock_global();
        }
        ble_hid_state.advertising = false;
        ble_hid_try_start_advertising_locked();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_hid_state.advertising = false;
        ble_hid_try_start_advertising_locked();
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io = {.action = event->passkey.params.action};
        if(io.action == BLE_SM_IOACT_DISP || io.action == BLE_SM_IOACT_INPUT) {
            io.passkey = BLE_HID_PASSKEY_DEFAULT;
            ESP_LOGI(TAG, "BLE passkey: %06" PRIu32, io.passkey);
        } else if(io.action == BLE_SM_IOACT_NUMCMP) {
            io.numcmp_accept = 1;
            ESP_LOGI(TAG, "BLE numeric comparison accepted: %06" PRIu32,
                     event->passkey.params.numcmp);
        }
        ble_sm_inject_io(event->passkey.conn_handle, &io);
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if(ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        ble_hid_unlock_global();
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }

    ble_hid_unlock_global();
    return 0;
}

static bool ble_hid_try_start_advertising_locked(void) {
    BleHid* hid = ble_hid_state.active;
    if(!hid || !ble_hid_state.ready || !ble_hid_state.advertising_requested ||
       ble_hid_state.advertising || hid->connected) {
        return false;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 0x20;
    params.itvl_max = 0x30;
    int rc = ble_gap_adv_start(
        nimble_glue_own_address_type(),
        NULL,
        BLE_HS_FOREVER,
        &params,
        ble_hid_gap_event,
        NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed, rc=%d", rc);
        return false;
    }
    ble_hid_state.advertising = true;
    return true;
}

static void ble_hid_on_sync(void* context) {
    (void)context;
    ble_hid_lock_global();
    ble_hid_state.ready = true;
    ble_hid_try_start_advertising_locked();
    ble_hid_unlock_global();
}

static bool ble_hid_has_custom_mac(const BleHidConfig* config) {
    static const uint8_t zero[6] = {0};
    return memcmp(config->mac, zero, sizeof(zero)) != 0;
}

void ble_hid_get_default_mac(uint8_t mac[6]) {
    if(esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        memset(mac, 0, 6);
        return;
    }
    esp_derive_local_mac(mac, mac);
    mac[0] = (mac[0] & 0x3fU) | 0xc0U;
}

BleHid* ble_hid_alloc(const BleHidConfig* config) {
    if(!config) return NULL;
    if(!ble_hid_state.mutex) {
        ble_hid_state.mutex = xSemaphoreCreateMutex();
        if(!ble_hid_state.mutex) return NULL;
    }

    ble_hid_lock_global();
    if(ble_hid_state.active) {
        ble_hid_unlock_global();
        return NULL;
    }
    ble_hid_unlock_global();

    BleHid* hid = calloc(1, sizeof(BleHid));
    if(!hid) return NULL;
    hid->mutex = xSemaphoreCreateMutex();
    if(!hid->mutex) {
        free(hid);
        return NULL;
    }
    memcpy(&hid->config, config, sizeof(*config));
    hid->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    hid->protocol_mode = 1;

    ble_hid_lock_global();
    ble_hid_state.active = hid;
    ble_hid_state.advertising = false;
    ble_hid_state.advertising_requested = false;
    ble_hid_state.ready = false;
    ble_hid_unlock_global();

    esp_err_t err = nimble_glue_init(config->device_name);
    if(err != ESP_OK) goto error;

    uint8_t io_cap = config->pairing == BleHidPairingModeVerifyYesNo ?
                         BLE_HS_IO_DISPLAY_YESNO :
                         BLE_HS_IO_DISPLAY_ONLY;
    nimble_glue_configure_security(config->bonding, true, true, io_cap);
    if(ble_hid_has_custom_mac(config)) {
        err = nimble_glue_set_random_address(config->mac);
        if(err != ESP_OK) goto error;
    }

    int rc = ble_gatts_count_cfg(ble_hid_services);
    if(rc == 0) rc = ble_gatts_add_svcs(ble_hid_services);
    if(rc != 0) {
        ESP_LOGE(TAG, "Failed to register HID services, rc=%d", rc);
        goto error;
    }

    err = nimble_glue_start(ble_hid_on_sync, NULL);
    if(err != ESP_OK) goto error;
    rc = ble_hid_configure_advertising(config->device_name);
    if(rc != 0) {
        ESP_LOGE(TAG, "Failed to configure HID advertising, rc=%d", rc);
        goto error;
    }

    ESP_LOGI(TAG, "NimBLE HID ready: %s", config->device_name);
    return hid;

error:
    nimble_glue_stop();
    ble_hid_lock_global();
    if(ble_hid_state.active == hid) ble_hid_state.active = NULL;
    ble_hid_state.ready = false;
    ble_hid_unlock_global();
    vSemaphoreDelete(hid->mutex);
    free(hid);
    return NULL;
}

void ble_hid_free(BleHid* hid) {
    if(!hid) return;
    ble_hid_stop_advertising();
    if(hid->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(hid->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    nimble_glue_stop();

    ble_hid_lock_global();
    if(ble_hid_state.active == hid) ble_hid_state.active = NULL;
    ble_hid_state.ready = false;
    ble_hid_unlock_global();
    vSemaphoreDelete(hid->mutex);
    free(hid);
}

void ble_hid_set_state_callback(BleHid* hid, BleHidStateCallback callback, void* context) {
    if(!hid) return;
    ble_hid_lock(hid);
    hid->state_callback = callback;
    hid->state_context = context;
    bool connected = hid->connected;
    ble_hid_unlock(hid);
    if(callback) callback(connected, context);
}

bool ble_hid_is_connected(BleHid* hid) {
    if(!hid) return false;
    ble_hid_lock(hid);
    bool connected = hid->connected;
    ble_hid_unlock(hid);
    return connected;
}

static bool ble_hid_send_report(BleHid* hid, uint8_t report_id, const void* data, size_t length) {
    if(!hid || !hid->connected || hid->conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
    uint16_t handle = report_id == BLE_HID_REPORT_ID_KEYBOARD ?
                          ble_hid_state.keyboard_input_handle :
                      report_id == BLE_HID_REPORT_ID_MOUSE ?
                          ble_hid_state.mouse_input_handle :
                          ble_hid_state.consumer_input_handle;
    if(hid->protocol_mode == 0) {
        if(report_id == BLE_HID_REPORT_ID_KEYBOARD) handle = ble_hid_state.boot_keyboard_input_handle;
        if(report_id == BLE_HID_REPORT_ID_MOUSE) handle = ble_hid_state.boot_mouse_input_handle;
    }
    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, length);
    if(!om) return false;
    return ble_gatts_notify_custom(hid->conn_handle, handle, om) == 0;
}

bool ble_hid_kb_press(BleHid* hid, uint16_t button) {
    if(!hid) return false;
    ble_hid_lock(hid);
    for(size_t i = 0; i < BLE_HID_KEYBOARD_KEYS_MAX; i++) {
        if(hid->keyboard_report.keys[i] == (button & 0xff)) break;
        if(hid->keyboard_report.keys[i] == 0) {
            hid->keyboard_report.keys[i] = button & 0xff;
            break;
        }
    }
    hid->keyboard_report.modifiers |= button >> 8;
    bool result = ble_hid_send_report(hid, BLE_HID_REPORT_ID_KEYBOARD,
                                      &hid->keyboard_report, sizeof(hid->keyboard_report));
    ble_hid_unlock(hid);
    return result;
}

bool ble_hid_kb_release(BleHid* hid, uint16_t button) {
    if(!hid) return false;
    ble_hid_lock(hid);
    for(size_t i = 0; i < BLE_HID_KEYBOARD_KEYS_MAX; i++) {
        if(hid->keyboard_report.keys[i] == (button & 0xff)) {
            hid->keyboard_report.keys[i] = 0;
            break;
        }
    }
    hid->keyboard_report.modifiers &= ~(button >> 8);
    bool result = ble_hid_send_report(hid, BLE_HID_REPORT_ID_KEYBOARD,
                                      &hid->keyboard_report, sizeof(hid->keyboard_report));
    ble_hid_unlock(hid);
    return result;
}

bool ble_hid_kb_release_all(BleHid* hid) {
    if(!hid) return false;
    ble_hid_lock(hid);
    memset(&hid->keyboard_report, 0, sizeof(hid->keyboard_report));
    bool result = ble_hid_send_report(hid, BLE_HID_REPORT_ID_KEYBOARD,
                                      &hid->keyboard_report, sizeof(hid->keyboard_report));
    ble_hid_unlock(hid);
    return result;
}

bool ble_hid_mouse_move(BleHid* hid, int8_t dx, int8_t dy) {
    if(!hid) return false;
    ble_hid_lock(hid);
    hid->mouse_report.x = dx;
    hid->mouse_report.y = dy;
    bool result = ble_hid_send_report(hid, BLE_HID_REPORT_ID_MOUSE,
                                      &hid->mouse_report, sizeof(hid->mouse_report));
    hid->mouse_report.x = 0;
    hid->mouse_report.y = 0;
    ble_hid_unlock(hid);
    return result;
}

bool ble_hid_mouse_press(BleHid* hid, uint8_t button) {
    if(!hid) return false;
    ble_hid_lock(hid);
    hid->mouse_report.buttons |= button;
    bool result = ble_hid_send_report(hid, BLE_HID_REPORT_ID_MOUSE,
                                      &hid->mouse_report, sizeof(hid->mouse_report));
    ble_hid_unlock(hid);
    return result;
}

bool ble_hid_mouse_release(BleHid* hid, uint8_t button) {
    if(!hid) return false;
    ble_hid_lock(hid);
    hid->mouse_report.buttons &= ~button;
    bool result = ble_hid_send_report(hid, BLE_HID_REPORT_ID_MOUSE,
                                      &hid->mouse_report, sizeof(hid->mouse_report));
    ble_hid_unlock(hid);
    return result;
}

bool ble_hid_mouse_release_all(BleHid* hid) {
    if(!hid) return false;
    ble_hid_lock(hid);
    memset(&hid->mouse_report, 0, sizeof(hid->mouse_report));
    bool result = ble_hid_send_report(hid, BLE_HID_REPORT_ID_MOUSE,
                                      &hid->mouse_report, sizeof(hid->mouse_report));
    ble_hid_unlock(hid);
    return result;
}

bool ble_hid_mouse_scroll(BleHid* hid, int8_t delta) {
    if(!hid) return false;
    ble_hid_lock(hid);
    hid->mouse_report.wheel = delta;
    bool result = ble_hid_send_report(hid, BLE_HID_REPORT_ID_MOUSE,
                                      &hid->mouse_report, sizeof(hid->mouse_report));
    hid->mouse_report.wheel = 0;
    ble_hid_unlock(hid);
    return result;
}

bool ble_hid_consumer_press(BleHid* hid, uint16_t button) {
    if(!hid) return false;
    ble_hid_lock(hid);
    hid->consumer_report.key = button;
    bool result = ble_hid_send_report(hid, BLE_HID_REPORT_ID_CONSUMER,
                                      &hid->consumer_report, sizeof(hid->consumer_report));
    ble_hid_unlock(hid);
    return result;
}

bool ble_hid_consumer_release(BleHid* hid, uint16_t button) {
    if(!hid) return false;
    ble_hid_lock(hid);
    if(hid->consumer_report.key == button) hid->consumer_report.key = 0;
    bool result = ble_hid_send_report(hid, BLE_HID_REPORT_ID_CONSUMER,
                                      &hid->consumer_report, sizeof(hid->consumer_report));
    ble_hid_unlock(hid);
    return result;
}

bool ble_hid_consumer_release_all(BleHid* hid) {
    if(!hid) return false;
    ble_hid_lock(hid);
    hid->consumer_report.key = 0;
    bool result = ble_hid_send_report(hid, BLE_HID_REPORT_ID_CONSUMER,
                                      &hid->consumer_report, sizeof(hid->consumer_report));
    ble_hid_unlock(hid);
    return result;
}

uint8_t ble_hid_get_led_state(BleHid* hid) {
    if(!hid) return 0;
    ble_hid_lock(hid);
    uint8_t state = hid->led_state;
    ble_hid_unlock(hid);
    return state;
}

bool ble_hid_start_advertising(void) {
    ble_hid_lock_global();
    bool accepted = ble_hid_state.active != NULL;
    if(accepted) {
        ble_hid_state.advertising_requested = true;
        ble_hid_try_start_advertising_locked();
    }
    ble_hid_unlock_global();
    return accepted;
}

void ble_hid_stop_advertising(void) {
    ble_hid_lock_global();
    ble_hid_state.advertising_requested = false;
    if(ble_hid_state.advertising) {
        ble_gap_adv_stop();
        ble_hid_state.advertising = false;
    }
    ble_hid_unlock_global();
}

bool ble_hid_is_advertising(void) {
    ble_hid_lock_global();
    bool advertising = ble_hid_state.advertising_requested && ble_hid_state.active &&
                       !ble_hid_state.active->connected;
    ble_hid_unlock_global();
    return advertising;
}

bool ble_hid_is_active(void) {
    ble_hid_lock_global();
    bool active = ble_hid_state.active != NULL;
    ble_hid_unlock_global();
    return active;
}

void ble_hid_reset_initialized(void) {
    if(!ble_hid_state.mutex) return;
    ble_hid_lock_global();
    ble_hid_state.ready = false;
    ble_hid_state.advertising = false;
    ble_hid_state.advertising_requested = false;
    ble_hid_state.active = NULL;
    ble_hid_unlock_global();
}

bool ble_hid_remove_pairing(void) {
    return nimble_glue_remove_all_bonds();
}
