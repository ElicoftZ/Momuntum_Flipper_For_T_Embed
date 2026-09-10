/**
 * @file chameleon.c
 * @brief NimBLE GATT-client transport + ChameleonUltra NUS protocol.
 *
 * The normal Flipper BLE profile is stopped while this client owns NimBLE.
 * The transport scans, connects, discovers Nordic UART Service, subscribes to
 * notifications, and exchanges ChameleonUltra protocol frames.
 */
#include "chameleon.h"

#include <esp_log.h>
#include <furi.h>
#include <btshim.h>
#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <nimble_glue.h>
#include <string.h>

#define TAG           "Chameleon"
#define CHAM_DEV_NAME "ChameleonUltra"

/* Nordic UART Service UUIDs, stored in NimBLE's little-endian UUID128 order:
 * 6E400001-B5A3-F393-E0A9-E50E24DCCA9E (service), -0002 TX(write), -0003 RX(notify) */
static const ble_uuid128_t NUS_SVC_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);
static const ble_uuid16_t CCCD_UUID = BLE_UUID16_INIT(0x2902);

/* HF14A_RAW option bits: b7 activateRfField, b6 waitResponse, b5 appendCrc,
 * b4 autoSelect, b3 keepRfField, b2 checkResponseCrc. Default = a normal
 * ISO14443-3A exchange with auto-anticollision and CRC handling. */
#define CHAMELEON_RAW_OPT_DEFAULT ((1 << 6) | (1 << 5) | (1 << 4) | (1 << 2)) /* 0x74 */

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_connected = false;
static bool s_hal_started = false;
/* True if BT was disabled in settings when we connected: we force-started the
 * stack so the ESP controller gets initialized, and must disable it again on
 * disconnect to honor the user's setting. */
static bool s_bt_was_disabled = false;

static volatile bool s_scanning = false;
static volatile bool s_dev_found = false;
static ble_addr_t s_dev_addr;

static volatile bool s_search_done = false;
static volatile bool s_chars_done = false;
static volatile bool s_dsc_done = false;
static uint16_t s_svc_start = 0;
static uint16_t s_svc_end = 0;
static uint16_t s_write_handle = 0;
static uint16_t s_notify_handle = 0;
static uint16_t s_cccd_handle = 0;
static volatile bool s_cccd_done = false;
static volatile bool s_cccd_written = false;

static int s_device_mode = -1; /* cache to skip redundant CHANGE_DEVICE_MODE */

/* RX frame reassembly + last parsed response (commands are serialized) */
static uint8_t s_acc[CHAMELEON_RESP_DATA_MAX + 32];
static size_t s_acc_len = 0;
static ChameleonResp s_last_resp;
static volatile bool s_resp_ready = false;

static uint8_t cham_lrc(const uint8_t* d, size_t n) {
    uint8_t s = 0;
    for(size_t i = 0; i < n; i++) s += d[i];
    return (uint8_t)((0x100 - (s & 0xff)) & 0xff);
}

/* Try to extract one complete frame from the accumulation buffer. */
static void cham_try_parse(void) {
    /* Resync to SOF 0x11 0xEF */
    while(s_acc_len >= 2 && !(s_acc[0] == 0x11 && s_acc[1] == 0xEF)) {
        memmove(s_acc, s_acc + 1, --s_acc_len);
    }
    if(s_acc_len < 10) return;

    uint16_t data_len = ((uint16_t)s_acc[6] << 8) | s_acc[7];
    if(data_len > CHAMELEON_RESP_DATA_MAX) {
        /* Bogus length — drop SOF and resync */
        memmove(s_acc, s_acc + 2, s_acc_len -= 2);
        return;
    }
    size_t frame_len = 10u + data_len;
    if(s_acc_len < frame_len) return;

    s_last_resp.command = ((uint16_t)s_acc[2] << 8) | s_acc[3];
    s_last_resp.status = s_acc[5];
    s_last_resp.data_len = data_len;
    if(data_len) memcpy(s_last_resp.data, &s_acc[9], data_len);
    s_resp_ready = true;

    /* Consume the frame; keep any trailing bytes for the next parse */
    size_t rest = s_acc_len - frame_len;
    if(rest) memmove(s_acc, s_acc + frame_len, rest);
    s_acc_len = rest;
}

/* ------------------------------------------------------------ NimBLE GAP --- */

static void cham_parse_name(const uint8_t* data, uint8_t data_len, char name[33]) {
    name[0] = '\0';
    uint8_t pos = 0;
    while(pos + 1 < data_len) {
        uint8_t len = data[pos];
        if(len == 0 || pos + len >= data_len) break;
        uint8_t type = data[pos + 1];
        if(type == BLE_HS_ADV_TYPE_COMP_NAME || type == BLE_HS_ADV_TYPE_INCOMP_NAME) {
            uint8_t name_len = len - 1;
            if(name_len > 32) name_len = 32;
            memcpy(name, data + pos + 2, name_len);
            name[name_len] = '\0';
            return;
        }
        pos += len + 1;
    }
}

static void cham_receive_notification(struct os_mbuf* om) {
    uint16_t incoming_len = OS_MBUF_PKTLEN(om);
    if(incoming_len == 0) return;

    size_t space = sizeof(s_acc) - s_acc_len;
    if(incoming_len > space) {
        s_acc_len = 0;
        space = sizeof(s_acc);
    }
    uint16_t copy_len = incoming_len > space ? (uint16_t)space : incoming_len;
    uint16_t copied = 0;
    if(ble_hs_mbuf_to_flat(om, s_acc + s_acc_len, copy_len, &copied) == 0) {
        s_acc_len += copied;
        cham_try_parse();
    }
}

static int cham_gap_cb(struct ble_gap_event* event, void* context) {
    (void)context;
    switch(event->type) {
    case BLE_GAP_EVENT_DISC:
        if(!s_dev_found) {
            char name[33];
            cham_parse_name(event->disc.data, event->disc.length_data, name);
            if(strcmp(name, CHAM_DEV_NAME) == 0) {
                s_dev_addr = event->disc.addr;
                s_dev_found = true;
                ble_gap_disc_cancel();
            }
        }
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
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_connected = false;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_connected = false;
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if(event->notify_rx.attr_handle == s_notify_handle) {
            cham_receive_notification(event->notify_rx.om);
        }
        break;
    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------- NimBLE GATT --- */

static int cham_service_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    const struct ble_gatt_svc* service,
    void* context) {
    (void)conn_handle;
    (void)context;
    if(error->status == 0 && service) {
        if(ble_uuid_cmp(&service->uuid.u, &NUS_SVC_UUID.u) == 0) {
            s_svc_start = service->start_handle;
            s_svc_end = service->end_handle;
        }
    } else {
        s_search_done = true;
    }
    return 0;
}

static int cham_char_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    const struct ble_gatt_chr* characteristic,
    void* context) {
    (void)conn_handle;
    (void)context;
    if(error->status == 0 && characteristic) {
        if(ble_uuid_cmp(&characteristic->uuid.u, &NUS_TX_UUID.u) == 0) {
            s_write_handle = characteristic->val_handle;
        } else if(ble_uuid_cmp(&characteristic->uuid.u, &NUS_RX_UUID.u) == 0) {
            s_notify_handle = characteristic->val_handle;
        }
    } else {
        s_chars_done = true;
    }
    return 0;
}

static int cham_dsc_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    uint16_t chr_val_handle,
    const struct ble_gatt_dsc* descriptor,
    void* context) {
    (void)conn_handle;
    (void)context;
    if(error->status == 0 && descriptor && chr_val_handle == s_notify_handle &&
       ble_uuid_cmp(&descriptor->uuid.u, &CCCD_UUID.u) == 0) {
        s_cccd_handle = descriptor->handle;
    } else if(error->status != 0) {
        s_dsc_done = true;
    }
    return 0;
}

static int cham_cccd_write_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    struct ble_gatt_attr* attr,
    void* context) {
    (void)conn_handle;
    (void)attr;
    (void)context;
    s_cccd_written = error->status == 0;
    s_cccd_done = true;
    return 0;
}

static int cham_write_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    struct ble_gatt_attr* attr,
    void* context) {
    (void)conn_handle;
    (void)attr;
    (void)context;
    if(error->status != 0) ESP_LOGW(TAG, "GATT write failed: %d", error->status);
    return 0;
}

/* --------------------------------------------------- HAL start / stop ---- */

static bool cham_hal_start(void) {
    if(s_hal_started) return true;

    Bt* bt = furi_record_open(RECORD_BT);
    s_bt_was_disabled = !bt_is_enabled(bt);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    furi_delay_ms(50);

    esp_err_t err = nimble_glue_init("Chameleon Client");
    if(err != ESP_OK) return false;
    nimble_glue_configure_security(false, false, false, BLE_HS_IO_NO_INPUT_OUTPUT);
    ble_att_set_preferred_mtu(247);
    err = nimble_glue_start(NULL, NULL);
    if(err != ESP_OK) {
        nimble_glue_stop();
        return false;
    }

    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_hal_started = true;
    return true;
}

static void cham_hal_stop(void) {
    if(s_scanning) {
        ble_gap_disc_cancel();
        for(int i = 0; i < 20 && s_scanning; i++) furi_delay_ms(10);
    }
    if(s_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        for(int i = 0; i < 40 && s_connected; i++) furi_delay_ms(25);
    }
    nimble_glue_stop();

    Bt* bt = furi_record_open(RECORD_BT);
    if(!s_bt_was_disabled) bt_start_stack(bt);
    furi_record_close(RECORD_BT);

    s_bt_was_disabled = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_connected = false;
    s_scanning = false;
    s_hal_started = false;
    s_device_mode = -1;
}

/* ----------------------------------------------------- public API -------- */

bool chameleon_connect(volatile bool* abort_flag) {
    if(s_connected) return true;
    if(!cham_hal_start()) {
        ESP_LOGE(TAG, "HAL start failed");
        cham_hal_stop();
        return false;
    }

    /* Scan for the device by name */
    s_dev_found = false;
    s_connected = false;
    s_search_done = false;
    s_chars_done = false;
    s_dsc_done = false;
    s_cccd_done = false;
    s_cccd_written = false;
    s_svc_start = s_svc_end = s_write_handle = s_notify_handle = s_cccd_handle = 0;
    s_acc_len = 0;
    s_resp_ready = false;

    struct ble_gap_disc_params scan_params = {
        .passive = 0,
        .itvl = 0x50,
        .window = 0x30,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(
        nimble_glue_own_address_type(), BLE_HS_FOREVER, &scan_params, cham_gap_cb, NULL);
    if(rc != 0) {
        ESP_LOGW(TAG, "scan start failed: %d", rc);
        cham_hal_stop();
        return false;
    }
    s_scanning = true;

    for(int i = 0; i < 300 && !s_dev_found; i++) { /* up to ~15 s */
        if(abort_flag && *abort_flag) {
            ble_gap_disc_cancel();
            cham_hal_stop();
            return false;
        }
        furi_delay_ms(50);
    }
    if(!s_dev_found) {
        ESP_LOGW(TAG, "device not found");
        cham_hal_stop();
        return false;
    }
    if(s_scanning) ble_gap_disc_cancel();
    for(int i = 0; i < 20 && s_scanning; i++) furi_delay_ms(10);

    /* Connect */
    rc = ble_gap_connect(
        nimble_glue_own_address_type(), &s_dev_addr, 5000, NULL, cham_gap_cb, NULL);
    if(rc != 0) {
        ESP_LOGW(TAG, "connect start failed: %d", rc);
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < 100 && !s_connected; i++) {
        if(abort_flag && *abort_flag) {
            ble_gap_conn_cancel();
            cham_hal_stop();
            return false;
        }
        furi_delay_ms(50);
    }
    if(!s_connected) {
        ESP_LOGW(TAG, "connect timeout");
        cham_hal_stop();
        return false;
    }

    /* Discover services and match Nordic UART Service. */
    s_search_done = false;
    rc = ble_gattc_disc_all_svcs(s_conn_handle, cham_service_cb, NULL);
    if(rc != 0) s_search_done = true;
    for(int i = 0; i < 150 && !s_search_done; i++) furi_delay_ms(20);
    if(!s_search_done) {
        ESP_LOGW(TAG, "service discovery timeout");
        cham_hal_stop();
        return false;
    }
    if(s_svc_start == 0) {
        ESP_LOGW(TAG, "NUS service not found");
        cham_hal_stop();
        return false;
    }
    ESP_LOGD(TAG, "NUS svc handles %u..%u", s_svc_start, s_svc_end);

    /* Find TX (write) + RX (notify) characteristic handles */
    s_chars_done = false;
    rc = ble_gattc_disc_all_chrs(
        s_conn_handle, s_svc_start, s_svc_end, cham_char_cb, NULL);
    if(rc != 0) {
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < 100 && !s_chars_done; i++) furi_delay_ms(20);
    if(s_write_handle == 0 || s_notify_handle == 0) {
        ESP_LOGW(TAG, "NUS chars missing (w=%u n=%u)", s_write_handle, s_notify_handle);
        cham_hal_stop();
        return false;
    }

    /* Discover and write the notification CCCD. */
    s_dsc_done = false;
    rc = ble_gattc_disc_all_dscs(
        s_conn_handle, s_notify_handle, s_svc_end, cham_dsc_cb, NULL);
    if(rc != 0) {
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < 100 && !s_dsc_done; i++) furi_delay_ms(20);
    if(s_cccd_handle == 0) {
        ESP_LOGW(TAG, "CCCD descriptor not found");
        cham_hal_stop();
        return false;
    }

    uint8_t cccd_value[2] = {0x01, 0x00};
    s_cccd_done = false;
    s_cccd_written = false;
    rc = ble_gattc_write_flat(
        s_conn_handle,
        s_cccd_handle,
        cccd_value,
        sizeof(cccd_value),
        cham_cccd_write_cb,
        NULL);
    if(rc != 0) {
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < 60 && !s_cccd_done; i++) furi_delay_ms(20);
    if(!s_cccd_written) {
        ESP_LOGW(TAG, "CCCD write failed");
        cham_hal_stop();
        return false;
    }

    ESP_LOGD(TAG, "connected & subscribed");
    return true;
}

void chameleon_disconnect(void) {
    if(!s_hal_started) return;
    cham_hal_stop();
    s_connected = false;
}

bool chameleon_is_connected(void) {
    return s_connected;
}

bool chameleon_cmd(
    uint16_t cmd,
    const uint8_t* data,
    uint16_t len,
    ChameleonResp* out,
    uint32_t timeout_ms) {
    if(!s_connected || s_write_handle == 0) return false;
    if(len > CHAMELEON_RESP_DATA_MAX) return false;

    uint8_t frame[CHAMELEON_RESP_DATA_MAX + 16];
    frame[0] = 0x11;
    frame[1] = 0xEF;
    frame[2] = (cmd >> 8) & 0xFF;
    frame[3] = cmd & 0xFF;
    frame[4] = 0x00;
    frame[5] = 0x00;
    frame[6] = (len >> 8) & 0xFF;
    frame[7] = len & 0xFF;
    frame[8] = cham_lrc(&frame[2], 6);
    if(len) memcpy(&frame[9], data, len);
    frame[9 + len] = cham_lrc(&frame[9], len);

    s_resp_ready = false;
    s_acc_len = 0;

    int rc = ble_gattc_write_flat(
        s_conn_handle, s_write_handle, frame, 10 + len, cham_write_cb, NULL);
    ESP_LOGD(TAG, "cmd %u write -> %d (wh=%u)", cmd, rc, s_write_handle);
    if(rc != 0) {
        ESP_LOGW(TAG, "write_char: %d", rc);
        return false;
    }

    uint32_t waited = 0;
    while(!s_resp_ready && waited < timeout_ms) {
        furi_delay_ms(10);
        waited += 10;
    }
    if(!s_resp_ready) {
        ESP_LOGW(TAG, "cmd %u TIMEOUT (no notify in %lums)", cmd, (unsigned long)timeout_ms);
        return false;
    }
    ESP_LOGD(
        TAG,
        "cmd %u resp: rxcmd=%u status=%02X len=%u",
        cmd,
        s_last_resp.command,
        s_last_resp.status,
        s_last_resp.data_len);
    if(out) *out = s_last_resp;
    return true;
}

bool chameleon_mf1_write_block(
    uint8_t block,
    uint8_t key_type,
    const uint8_t key[6],
    const uint8_t data16[16]) {
    uint8_t payload[2 + 6 + 16];
    payload[0] = key_type; /* 0x60 = key A, 0x61 = key B */
    payload[1] = block;
    memcpy(&payload[2], key, 6);
    memcpy(&payload[8], data16, 16);

    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdMf1WriteOneBlock, payload, sizeof(payload), &r, 1500))
        return false;
    return r.status == CHAMELEON_STATUS_HF_TAG_OK;
}

bool chameleon_mfu_write_page(uint8_t page, const uint8_t data4[4]) {
    /* HF14A_RAW WRITE (0xA2 page d0 d1 d2 d3); the card answers a 4-bit ACK,
     * so accept HF_TAG_OK regardless of returned data length. */
    uint8_t frame[6] = {0xA2, page, data4[0], data4[1], data4[2], data4[3]};
    uint8_t payload[5 + sizeof(frame)];
    payload[0] = CHAMELEON_RAW_OPT_DEFAULT;
    payload[1] = 0x00;
    payload[2] = 200;
    payload[3] = 0x00;
    payload[4] = (uint8_t)(sizeof(frame) * 8);
    memcpy(&payload[5], frame, sizeof(frame));

    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdHf14aRaw, payload, sizeof(payload), &r, 1500))
        return false;
    return r.status == CHAMELEON_STATUS_HF_TAG_OK;
}

static bool chameleon_cmd_ok(uint16_t cmd, const uint8_t* d, uint16_t n) {
    ChameleonResp r;
    if(!chameleon_cmd(cmd, d, n, &r, 1500)) return false;
    return r.status == CHAMELEON_STATUS_SUCCESS;
}

bool chameleon_slot_select(uint8_t slot, uint16_t tag_type) {
    if(slot < 1 || slot > 8) return false;
    uint8_t s = slot - 1;

    uint8_t en[3] = {s, 0x02 /* HF */, 0x01};
    if(!chameleon_cmd_ok(ChameleonCmdSetSlotEnable, en, sizeof(en))) return false;

    uint8_t act[1] = {s};
    if(!chameleon_cmd_ok(ChameleonCmdSetActiveSlot, act, sizeof(act))) return false;

    uint8_t ty[3] = {s, (uint8_t)(tag_type >> 8), (uint8_t)(tag_type & 0xFF)};
    if(!chameleon_cmd_ok(ChameleonCmdSetSlotTagType, ty, sizeof(ty))) return false;

    /* Initialise the slot to defaults for this tag type — required before
     * loading emulation data, otherwise the emulated card stays inert. */
    if(!chameleon_cmd_ok(ChameleonCmdSetSlotDataDefault, ty, sizeof(ty))) return false;

    return true;
}

bool chameleon_slot_config_save(void) {
    return chameleon_cmd_ok(ChameleonCmdSlotDataConfigSave, NULL, 0);
}

bool chameleon_mf1_eload(uint8_t start_block, const uint8_t* data, uint16_t nblocks) {
    /* Keep each BLE write well under the ATT MTU: 8 blocks (128 B) per chunk */
    const uint16_t chunk_blocks = 8;
    uint8_t buf[1 + chunk_blocks * 16];
    for(uint16_t b = 0; b < nblocks; b += chunk_blocks) {
        uint16_t cnt = (nblocks - b < chunk_blocks) ? (nblocks - b) : chunk_blocks;
        buf[0] = (uint8_t)(start_block + b);
        memcpy(&buf[1], &data[(size_t)b * 16], (size_t)cnt * 16);
        if(!chameleon_cmd_ok(ChameleonCmdMf1WriteEmuBlockData, buf, 1 + cnt * 16))
            return false;
    }
    return true;
}

bool chameleon_mfu_eload(uint8_t start_page, const uint8_t* data, uint16_t npages) {
    /* MF0_NTAG_WRITE_EMU_PAGE_DATA payload is [page_start, page_count, data]
     * (unlike MF1_WRITE_EMU_BLOCK_DATA which is just [block, data]). Omitting
     * the count byte shifts all pages by one and the clone reads back wrong. */
    const uint16_t chunk_pages = 30; /* 2 + 120 B */
    uint8_t buf[2 + chunk_pages * 4];
    for(uint16_t p = 0; p < npages; p += chunk_pages) {
        uint16_t cnt = (npages - p < chunk_pages) ? (npages - p) : chunk_pages;
        buf[0] = (uint8_t)(start_page + p);
        buf[1] = (uint8_t)cnt;
        memcpy(&buf[2], &data[(size_t)p * 4], (size_t)cnt * 4);
        if(!chameleon_cmd_ok(ChameleonCmdMf0NtagWriteEmuPageData, buf, 2 + cnt * 4))
            return false;
    }
    return true;
}

bool chameleon_set_anticoll(
    const uint8_t* uid,
    uint8_t uid_len,
    const uint8_t atqa[2],
    uint8_t sak) {
    if(uid_len != 4 && uid_len != 7) return false;
    uint8_t p[1 + 10 + 4];
    uint8_t i = 0;
    p[i++] = uid_len;
    memcpy(&p[i], uid, uid_len);
    i += uid_len;
    p[i++] = atqa[1];
    p[i++] = atqa[0];
    p[i++] = sak;
    p[i++] = 0x00; /* ATS length */
    return chameleon_cmd_ok(ChameleonCmdHf14aSetAntiCollData, p, i);
}

bool chameleon_get_app_version(uint8_t* major, uint8_t* minor) {
    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdGetAppVersion, NULL, 0, &r, 1000)) return false;
    if(r.status != CHAMELEON_STATUS_SUCCESS || r.data_len < 2) return false;
    if(major) *major = r.data[1]; /* device sends [minor, major] */
    if(minor) *minor = r.data[0];
    return true;
}

bool chameleon_get_battery(uint16_t* millivolt, uint8_t* percent) {
    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdGetBatteryInfo, NULL, 0, &r, 1000)) return false;
    if(r.status != CHAMELEON_STATUS_SUCCESS || r.data_len < 3) return false;
    if(millivolt) *millivolt = ((uint16_t)r.data[0] << 8) | r.data[1];
    if(percent) *percent = r.data[2];
    return true;
}

bool chameleon_set_device_mode(uint8_t mode) {
    if(s_device_mode == (int)mode) return true;
    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdChangeMode, &mode, 1, &r, 1000)) return false;
    if(r.status != CHAMELEON_STATUS_SUCCESS) return false;
    s_device_mode = (int)mode;
    return true;
}

bool chameleon_hf14a_scan(uint8_t* uid, uint8_t* uid_len, uint8_t atqa[2], uint8_t* sak) {
    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdHf14aScan, NULL, 0, &r, 1000)) return false;
    if(r.status != CHAMELEON_STATUS_HF_TAG_OK || r.data_len < 5) return false;
    /* Response data: [uid_len][uid...][atqa1][atqa0][sak] */
    uint8_t ul = r.data[0];
    if(ul == 0 || ul > 10 || (size_t)(4 + ul) > r.data_len) return false;
    if(uid) memcpy(uid, &r.data[1], ul);
    if(uid_len) *uid_len = ul;
    if(atqa) {
        atqa[1] = r.data[1 + ul];
        atqa[0] = r.data[2 + ul];
    }
    if(sak) *sak = r.data[3 + ul];
    return true;
}

bool chameleon_mf1_read_block(
    uint8_t block,
    uint8_t key_type,
    const uint8_t key[6],
    uint8_t out16[16]) {
    uint8_t payload[8];
    payload[0] = key_type; /* 0x60 = key A, 0x61 = key B */
    payload[1] = block;
    memcpy(&payload[2], key, 6);

    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdMf1ReadOneBlock, payload, sizeof(payload), &r, 1200))
        return false;
    if(r.status != CHAMELEON_STATUS_HF_TAG_OK || r.data_len < 16) return false;
    memcpy(out16, r.data, 16);
    return true;
}

bool chameleon_hf14a_raw(
    const uint8_t* data,
    uint8_t len,
    uint8_t* out,
    uint16_t out_cap,
    uint16_t* out_len) {
    if(len == 0 || len > 240) return false;

    uint8_t payload[5 + 240];
    payload[0] = CHAMELEON_RAW_OPT_DEFAULT;
    payload[1] = 0x00;
    payload[2] = 200; /* timeout */
    payload[3] = 0x00;
    payload[4] = (uint8_t)(len * 8); /* bitlen */
    memcpy(&payload[5], data, len);

    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdHf14aRaw, payload, 5 + len, &r, 1200)) return false;
    if(r.status != CHAMELEON_STATUS_HF_TAG_OK || r.data_len == 0) return false;
    uint16_t n = r.data_len;
    if(n > out_cap) n = out_cap;
    memcpy(out, r.data, n);
    if(out_len) *out_len = n;
    return true;
}
