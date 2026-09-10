#include "ble_walk_hal.h"
#include "whisper_pair_ad.h"

#include <string.h>

#include <esp_log.h>
#include <furi.h>
#include <btshim.h>
#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <nimble_glue.h>

#define TAG "BleWalkHal"

static BleWalkDevice s_devices[BLE_WALK_MAX_DEVICES];
static uint16_t s_device_count;
static volatile bool s_scanning;
static bool s_fast_pair_only;

static BleWalkService s_services[BLE_WALK_MAX_SERVICES];
static uint16_t s_service_count;
static volatile bool s_services_ready;

static BleWalkChar s_chars[BLE_WALK_MAX_CHARS];
static uint16_t s_char_count;
static volatile bool s_chars_ready;

static uint8_t s_read_buf[BLE_WALK_MAX_VALUE_LEN];
static uint16_t s_read_len;
static volatile bool s_read_ready;
static volatile uint8_t s_read_status;
static volatile bool s_write_ready;
static int s_write_status;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_connected;
static bool s_hal_started;
static ble_addr_t s_last_connect_addr;

static void walk_uuid_from_nimble(BleWalkUuid* destination, const ble_uuid_t* source) {
    memset(destination, 0, sizeof(*destination));
    if(source->type == BLE_UUID_TYPE_16) {
        destination->len = BLE_WALK_UUID_LEN_16;
        destination->uuid.uuid16 = BLE_UUID16(source)->value;
    } else if(source->type == BLE_UUID_TYPE_32) {
        destination->len = BLE_WALK_UUID_LEN_32;
        destination->uuid.uuid32 = BLE_UUID32(source)->value;
    } else if(source->type == BLE_UUID_TYPE_128) {
        destination->len = BLE_WALK_UUID_LEN_128;
        memcpy(destination->uuid.uuid128, BLE_UUID128(source)->value, 16);
    }
}

static void walk_addr_to_display(uint8_t destination[6], const ble_addr_t* source) {
    for(size_t i = 0; i < 6; i++) destination[i] = source->val[5 - i];
}

static ble_addr_t walk_addr_from_display(const uint8_t source[6], uint8_t type) {
    ble_addr_t result = {.type = type};
    for(size_t i = 0; i < 6; i++) result.val[i] = source[5 - i];
    return result;
}

static void walk_parse_name(const uint8_t* data, uint8_t data_len, char name[32]) {
    uint8_t pos = 0;
    while(pos < data_len) {
        uint8_t len = data[pos];
        if(len == 0 || pos + len >= data_len) break;
        uint8_t type = data[pos + 1];
        if(type == 0x09 || type == 0x08) {
            uint8_t name_len = len - 1;
            if(name_len > 31) name_len = 31;
            memcpy(name, data + pos + 2, name_len);
            name[name_len] = '\0';
            return;
        }
        pos += len + 1;
    }
}

static void walk_store_scan_result(const struct ble_gap_disc_desc* result) {
    if(s_fast_pair_only && !whisper_pair_parse_ad(result->data, result->length_data).present) return;
    uint8_t display_addr[6];
    walk_addr_to_display(display_addr, &result->addr);
    char parsed_name[32] = "";
    walk_parse_name(result->data, result->length_data, parsed_name);

    int found = -1;
    for(int i = 0; i < s_device_count; i++) {
        if(memcmp(s_devices[i].addr, display_addr, 6) == 0) {
            found = i;
            break;
        }
    }
    if(found < 0 && s_device_count < BLE_WALK_MAX_DEVICES) {
        found = s_device_count++;
        memset(&s_devices[found], 0, sizeof(s_devices[found]));
        memcpy(s_devices[found].addr, display_addr, 6);
        s_devices[found].addr_type = result->addr.type;
    }
    if(found < 0) return;

    BleWalkDevice* device = &s_devices[found];
    device->rssi = result->rssi;
    if(device->name[0] == '\0' && parsed_name[0] != '\0') {
        memcpy(device->name, parsed_name, sizeof(device->name));
    }
    uint8_t copy_len = result->length_data > 31 ? 31 : result->length_data;
    if(result->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
        device->scan_rsp_len = copy_len;
        memcpy(device->scan_rsp_data, result->data, copy_len);
    } else {
        device->adv_data_len = copy_len;
        memcpy(device->adv_data, result->data, copy_len);
    }
}

static int walk_gap_event_handler(struct ble_gap_event* event, void* context) {
    (void)context;
    switch(event->type) {
    case BLE_GAP_EVENT_DISC:
        walk_store_scan_result(&event->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        break;
    case BLE_GAP_EVENT_CONNECT:
        if(event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);
        } else {
            s_connected = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        break;
    default:
        break;
    }
    return 0;
}

static void walk_on_sync(void* context) {
    (void)context;
}

bool ble_walk_hal_start(void) {
    if(s_hal_started) return true;
    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);

    esp_err_t err = nimble_glue_init("BLE Walk");
    if(err != ESP_OK) return false;
    nimble_glue_configure_security(false, false, false, BLE_HS_IO_NO_INPUT_OUTPUT);
    ble_att_set_preferred_mtu(200);
    err = nimble_glue_start(walk_on_sync, NULL);
    if(err != ESP_OK) {
        nimble_glue_stop();
        return false;
    }

    memset(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;
    s_scanning = false;
    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_hal_started = true;
    return true;
}

void ble_walk_hal_stop(void) {
    ble_walk_hal_stop_scan();
    ble_walk_hal_disconnect();
    nimble_glue_stop();

    Bt* bt = furi_record_open(RECORD_BT);
    bt_start_stack(bt);
    furi_record_close(RECORD_BT);
    s_hal_started = false;
}

static bool walk_start_scan(bool passive, bool fast_pair_only) {
    s_fast_pair_only = fast_pair_only;
    struct ble_gap_disc_params params = {
        .passive = passive,
        .itvl = 0x50,
        .window = 0x30,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(
        nimble_glue_own_address_type(),
        BLE_HS_FOREVER,
        &params,
        walk_gap_event_handler,
        NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed, rc=%d", rc);
        return false;
    }
    s_scanning = true;
    return true;
}

bool ble_walk_hal_start_scan(void) {
    return walk_start_scan(false, false);
}

bool ble_walk_hal_start_passive_scan(bool fast_pair_only) {
    ble_walk_hal_stop_scan();
    memset(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;
    return walk_start_scan(true, fast_pair_only);
}

void ble_walk_hal_stop_scan(void) {
    if(s_scanning) ble_gap_disc_cancel();
    s_scanning = false;
}

bool ble_walk_hal_is_scanning(void) {
    return s_scanning;
}

BleWalkDevice* ble_walk_hal_get_devices(uint16_t* count) {
    *count = s_device_count;
    return s_devices;
}

bool ble_walk_hal_connect(BleWalkDevice* device, volatile bool* abort_flag) {
    if(!device || !s_hal_started) return false;
    ble_walk_hal_stop_scan();
    s_connected = false;
    s_service_count = 0;
    s_services_ready = false;
    s_last_connect_addr = walk_addr_from_display(device->addr, device->addr_type);

    int rc = ble_gap_connect(
        nimble_glue_own_address_type(),
        &s_last_connect_addr,
        4000,
        NULL,
        walk_gap_event_handler,
        NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed, rc=%d", rc);
        return false;
    }

    bool aborted = false;
    for(int i = 0; i < 80 && !s_connected; i++) {
        if(abort_flag && *abort_flag) {
            aborted = true;
            break;
        }
        furi_delay_ms(50);
    }
    if(!s_connected) {
        ble_gap_conn_cancel();
        if(!aborted) furi_delay_ms(100);
    }
    return s_connected;
}

void ble_walk_hal_disconnect(void) {
    if(s_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        for(int i = 0; i < 40 && s_connected; i++) furi_delay_ms(50);
    }
    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

bool ble_walk_hal_is_connected(void) {
    return s_connected;
}

static int walk_service_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    const struct ble_gatt_svc* service,
    void* arg) {
    (void)conn_handle;
    (void)arg;
    if(error->status == 0 && service && s_service_count < BLE_WALK_MAX_SERVICES) {
        BleWalkService* destination = &s_services[s_service_count++];
        walk_uuid_from_nimble(&destination->uuid, &service->uuid.u);
        destination->start_handle = service->start_handle;
        destination->end_handle = service->end_handle;
    } else if(error->status == BLE_HS_EDONE) {
        s_services_ready = true;
    } else if(error->status != 0) {
        s_services_ready = true;
    }
    return 0;
}

bool ble_walk_hal_discover_services(void) {
    if(!s_connected) return false;
    s_service_count = 0;
    s_services_ready = false;
    int rc = ble_gattc_disc_all_svcs(s_conn_handle, walk_service_cb, NULL);
    if(rc != 0) s_services_ready = true;
    return rc == 0;
}

bool ble_walk_hal_services_ready(void) {
    return s_services_ready;
}

BleWalkService* ble_walk_hal_get_services(uint16_t* count) {
    *count = s_service_count;
    return s_services;
}

static int walk_char_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    const struct ble_gatt_chr* characteristic,
    void* arg) {
    (void)conn_handle;
    (void)arg;
    if(error->status == 0 && characteristic && s_char_count < BLE_WALK_MAX_CHARS) {
        BleWalkChar* destination = &s_chars[s_char_count++];
        walk_uuid_from_nimble(&destination->uuid, &characteristic->uuid.u);
        destination->handle = characteristic->val_handle;
        destination->properties = characteristic->properties;
    } else if(error->status == BLE_HS_EDONE || error->status != 0) {
        s_chars_ready = true;
    }
    return 0;
}

bool ble_walk_hal_discover_chars(BleWalkService* service) {
    if(!s_connected || !service) return false;
    s_char_count = 0;
    s_chars_ready = false;
    int rc = ble_gattc_disc_all_chrs(
        s_conn_handle,
        service->start_handle,
        service->end_handle,
        walk_char_cb,
        NULL);
    if(rc != 0) s_chars_ready = true;
    return rc == 0;
}

bool ble_walk_hal_chars_ready(void) {
    return s_chars_ready;
}

BleWalkChar* ble_walk_hal_get_chars(uint16_t* count) {
    *count = s_char_count;
    return s_chars;
}

static int walk_read_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    struct ble_gatt_attr* attr,
    void* arg) {
    (void)conn_handle;
    (void)arg;
    s_read_status = error->status > UINT8_MAX ? UINT8_MAX : (uint8_t)error->status;
    s_read_len = 0;
    if(error->status == 0 && attr && attr->om) {
        ble_hs_mbuf_to_flat(attr->om, s_read_buf, sizeof(s_read_buf), &s_read_len);
    }
    s_read_ready = true;
    return 0;
}

bool ble_walk_hal_read_char(uint16_t handle) {
    if(!s_connected) return false;
    s_read_ready = false;
    s_read_len = 0;
    return ble_gattc_read(s_conn_handle, handle, walk_read_cb, NULL) == 0;
}

bool ble_walk_hal_read_ready(void) {
    return s_read_ready;
}

uint8_t* ble_walk_hal_get_read_value(uint16_t* len) {
    *len = s_read_len;
    return s_read_buf;
}

uint8_t ble_walk_hal_get_read_status(void) {
    return s_read_status;
}

static int walk_write_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    struct ble_gatt_attr* attr,
    void* arg) {
    (void)conn_handle;
    (void)attr;
    (void)arg;
    ESP_LOGI(TAG, "Write complete, status=%u", error->status);
    s_write_status = error->status;
    s_write_ready = true;
    return 0;
}

bool ble_walk_hal_write_char(uint16_t handle, const uint8_t* data, uint16_t len) {
    s_write_ready = false;
    s_write_status = BLE_HS_ENOTCONN;
    if(!s_connected || !data) return false;
    return ble_gattc_write_flat(
               s_conn_handle, handle, data, len, walk_write_cb, NULL) == 0;
}

bool ble_walk_hal_write_ready(void) {
    return s_write_ready;
}

int ble_walk_hal_get_write_status(void) {
    return s_write_status;
}
