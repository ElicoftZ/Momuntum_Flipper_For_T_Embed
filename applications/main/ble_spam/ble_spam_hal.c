#include "ble_spam_hal.h"

#include <string.h>

#include <esp_bt.h>
#include <esp_log.h>
#include <furi.h>
#include <furi_hal_random.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <nimble_glue.h>
#include <btshim.h>

#define TAG "BleSpamHal"

static volatile bool s_adv_configured = false;
static volatile bool s_advertising = false;

static struct ble_gap_adv_params s_adv_params = {
    .conn_mode = BLE_GAP_CONN_MODE_UND,
    .disc_mode = BLE_GAP_DISC_MODE_GEN,
    .itvl_min = 0x20,
    .itvl_max = 0x40,
};

static int spam_gap_event_handler(struct ble_gap_event* event, void* context) {
    (void)context;
    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_advertising = false;
        if(event->connect.status == 0) {
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        break;
    default:
        break;
    }
    return 0;
}

static void spam_on_sync(void* context) {
    (void)context;
}

bool ble_spam_hal_start(void) {
    ESP_LOGI(TAG, "Starting NimBLE spam HAL");
    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);

    esp_err_t err = nimble_glue_init("BLE");
    if(err != ESP_OK) return false;
    nimble_glue_configure_security(false, false, false, BLE_HS_IO_NO_INPUT_OUTPUT);
    err = nimble_glue_start(spam_on_sync, NULL);
    if(err != ESP_OK) {
        nimble_glue_stop();
        return false;
    }

#ifdef ESP_PWR_LVL_P21
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P21);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P21);
#else
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P20);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P20);
#endif

    s_adv_configured = false;
    s_advertising = false;
    ble_spam_hal_set_random_addr();
    return true;
}

void ble_spam_hal_stop(void) {
    ble_spam_hal_stop_adv();
    nimble_glue_stop();

    Bt* bt = furi_record_open(RECORD_BT);
    bt_start_stack(bt);
    furi_record_close(RECORD_BT);
}

bool ble_spam_hal_set_adv_data(const uint8_t* data, uint8_t len) {
    if(!data || len > BLE_HS_ADV_MAX_SZ) return false;
    if(s_advertising) ble_spam_hal_stop_adv();
    int rc = ble_gap_adv_set_data(data, len);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_data failed, rc=%d", rc);
        return false;
    }
    s_adv_configured = true;
    rc = ble_gap_adv_start(
        BLE_OWN_ADDR_RANDOM,
        NULL,
        BLE_HS_FOREVER,
        &s_adv_params,
        spam_gap_event_handler,
        NULL);
    s_advertising = rc == 0;
    if(rc != 0) ESP_LOGE(TAG, "ble_gap_adv_start failed, rc=%d", rc);
    return rc == 0;
}

void ble_spam_hal_stop_adv(void) {
    if(s_advertising || ble_gap_adv_active()) ble_gap_adv_stop();
    s_advertising = false;
}

static void ble_spam_hal_apply_address(const uint8_t display_order[6]) {
    uint8_t nimble_order[6];
    for(size_t i = 0; i < sizeof(nimble_order); i++) {
        nimble_order[i] = display_order[sizeof(nimble_order) - 1 - i];
    }
    nimble_order[5] = (nimble_order[5] & 0x3fU) | 0xc0U;
    int rc = ble_hs_id_set_rnd(nimble_order);
    if(rc != 0) ESP_LOGW(TAG, "ble_hs_id_set_rnd failed, rc=%d", rc);
}

void ble_spam_hal_set_random_addr(void) {
    uint8_t address[6];
    furi_hal_random_fill_buf(address, sizeof(address));
    address[0] = (address[0] & 0x3fU) | 0xc0U;
    ble_spam_hal_apply_address(address);
}

void ble_spam_hal_set_addr(const uint8_t addr[6]) {
    if(addr) ble_spam_hal_apply_address(addr);
}
