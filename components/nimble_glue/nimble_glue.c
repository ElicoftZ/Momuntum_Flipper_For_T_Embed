#include "nimble_glue.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <nvs_flash.h>

#include <host/ble_hs.h>
#include <host/ble_store.h>
#include <host/util/util.h>
#include <nimble/ble.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#define TAG "NimbleGlue"

/* Provided by the ESP-IDF NimBLE NVS store implementation. */
void ble_store_config_init(void);

static struct {
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t sync_sem;
    SemaphoreHandle_t host_stopped_sem;
    bool initialized;
    bool synced;
    bool host_started;
    bool random_address_requested;
    uint8_t random_address[6];
    uint8_t own_address_type;
    NimbleGlueSyncCallback sync_callback;
    void* sync_context;
} nimble_glue_state;

static void nimble_glue_lock(void) {
    if(nimble_glue_state.mutex) {
        xSemaphoreTake(nimble_glue_state.mutex, portMAX_DELAY);
    }
}

static void nimble_glue_unlock(void) {
    if(nimble_glue_state.mutex) {
        xSemaphoreGive(nimble_glue_state.mutex);
    }
}

static void nimble_glue_on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
    nimble_glue_lock();
    nimble_glue_state.synced = false;
    nimble_glue_unlock();
}

static void nimble_glue_on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if(rc != 0) {
        ESP_LOGE(TAG, "No usable BLE identity address, rc=%d", rc);
    }

    nimble_glue_lock();
    if(rc == 0 && nimble_glue_state.random_address_requested) {
        rc = ble_hs_id_set_rnd(nimble_glue_state.random_address);
        if(rc == 0) {
            nimble_glue_state.own_address_type = BLE_OWN_ADDR_RANDOM;
        } else {
            ESP_LOGE(TAG, "Failed to set random identity address, rc=%d", rc);
        }
    }

    if(rc == 0 && !nimble_glue_state.random_address_requested) {
        /* HID profiles use a random identity. After switching back to the
         * serial profile, infer_auto() could select that stale random identity
         * even though the reinitialized controller no longer had it set. The
         * next advertising command then failed with invalid HCI parameters.
         * ESP32 always has a public BT address, so select it explicitly for
         * profiles which did not request a custom random address. */
        uint8_t public_address[6];
        rc = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, public_address, NULL);
        if(rc == 0) {
            nimble_glue_state.own_address_type = BLE_OWN_ADDR_PUBLIC;
        } else {
            ESP_LOGE(TAG, "No public BLE identity address, rc=%d", rc);
        }
    }

    nimble_glue_state.synced = rc == 0;
    NimbleGlueSyncCallback callback = nimble_glue_state.sync_callback;
    void* context = nimble_glue_state.sync_context;
    SemaphoreHandle_t sync_sem = nimble_glue_state.sync_sem;
    nimble_glue_unlock();

    if(sync_sem) {
        xSemaphoreGive(sync_sem);
    }
    if(rc == 0 && callback) {
        callback(context);
    }
}

static void nimble_glue_host_task(void* context) {
    (void)context;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();

    /* The owner performs task deletion before nimble_port_deinit(). Doing the
     * deinit here raced the next profile start: the BT service could tear down
     * controller/GATT memory while this task was still returning from its stop
     * event, which made every BLE HID mode intermittently panic. */
    SemaphoreHandle_t stopped_sem = nimble_glue_state.host_stopped_sem;
    if(stopped_sem) xSemaphoreGive(stopped_sem);
    vTaskSuspend(NULL);
}

esp_err_t nimble_glue_init(const char* device_name) {
    if(!nimble_glue_state.mutex) {
        nimble_glue_state.mutex = xSemaphoreCreateMutex();
        if(!nimble_glue_state.mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    nimble_glue_lock();
    if(nimble_glue_state.initialized) {
        nimble_glue_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    nimble_glue_unlock();

    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if(err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if(err != ESP_OK) {
        return err;
    }

    err = nimble_port_init();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = nimble_glue_on_reset;
    ble_hs_cfg.sync_cb = nimble_glue_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_YESNO;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if(device_name && device_name[0]) {
        int rc = ble_svc_gap_device_name_set(device_name);
        if(rc != 0) {
            nimble_port_deinit();
            return ESP_ERR_INVALID_ARG;
        }
    }
    ble_store_config_init();

    nimble_glue_lock();
    nimble_glue_state.initialized = true;
    nimble_glue_state.synced = false;
    nimble_glue_state.host_started = false;
    nimble_glue_state.random_address_requested = false;
    nimble_glue_state.own_address_type = BLE_OWN_ADDR_PUBLIC;
    nimble_glue_state.sync_callback = NULL;
    nimble_glue_state.sync_context = NULL;
    nimble_glue_unlock();

    return ESP_OK;
}

void nimble_glue_configure_security(
    bool bonding,
    bool mitm,
    bool secure_connections,
    uint8_t io_capability) {
    ble_hs_cfg.sm_bonding = bonding ? 1 : 0;
    ble_hs_cfg.sm_mitm = mitm ? 1 : 0;
    ble_hs_cfg.sm_sc = secure_connections ? 1 : 0;
    ble_hs_cfg.sm_io_cap = io_capability;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
}

esp_err_t nimble_glue_set_random_address(const uint8_t address[6]) {
    if(!address) {
        return ESP_ERR_INVALID_ARG;
    }

    /* NimBLE stores controller addresses least-significant octet first. */
    uint8_t reversed[6];
    for(size_t i = 0; i < sizeof(reversed); i++) {
        reversed[i] = address[sizeof(reversed) - 1 - i];
    }
    reversed[5] = (reversed[5] & 0x3fU) | 0xc0U;

    nimble_glue_lock();
    if(nimble_glue_state.synced) {
        nimble_glue_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(nimble_glue_state.random_address, reversed, sizeof(reversed));
    nimble_glue_state.random_address_requested = true;
    nimble_glue_unlock();
    return ESP_OK;
}

esp_err_t nimble_glue_start(NimbleGlueSyncCallback callback, void* context) {
    nimble_glue_lock();
    if(!nimble_glue_state.initialized) {
        nimble_glue_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    if(!nimble_glue_state.sync_sem) {
        nimble_glue_state.sync_sem = xSemaphoreCreateBinary();
        if(!nimble_glue_state.sync_sem) {
            nimble_glue_unlock();
            return ESP_ERR_NO_MEM;
        }
    } else {
        xSemaphoreTake(nimble_glue_state.sync_sem, 0);
    }

    if(!nimble_glue_state.host_stopped_sem) {
        nimble_glue_state.host_stopped_sem = xSemaphoreCreateBinary();
        if(!nimble_glue_state.host_stopped_sem) {
            nimble_glue_unlock();
            return ESP_ERR_NO_MEM;
        }
    } else {
        xSemaphoreTake(nimble_glue_state.host_stopped_sem, 0);
    }

    nimble_glue_state.sync_callback = callback;
    nimble_glue_state.sync_context = context;
    SemaphoreHandle_t sync_sem = nimble_glue_state.sync_sem;
    nimble_glue_state.host_started = true;
    nimble_glue_unlock();

    nimble_port_freertos_init(nimble_glue_host_task);
    if(xSemaphoreTake(sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for NimBLE host sync");
        nimble_glue_stop();
        return ESP_ERR_TIMEOUT;
    }

    return nimble_glue_is_synced() ? ESP_OK : ESP_FAIL;
}

esp_err_t nimble_glue_stop(void) {
    nimble_glue_lock();
    bool initialized = nimble_glue_state.initialized;
    bool host_started = nimble_glue_state.host_started;
    SemaphoreHandle_t host_stopped_sem = nimble_glue_state.host_stopped_sem;
    nimble_glue_unlock();
    if(!initialized) {
        return ESP_OK;
    }

    if(host_started) {
        int rc = nimble_port_stop();
        if(rc != 0) {
            ESP_LOGE(TAG, "nimble_port_stop failed, rc=%d", rc);
            return ESP_FAIL;
        }

        if(!host_stopped_sem ||
           xSemaphoreTake(host_stopped_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "Timed out waiting for NimBLE host task to stop");
            return ESP_ERR_TIMEOUT;
        }

        /* The host task is suspended after leaving nimble_port_run(), so it can
         * now be deleted without touching a live NimBLE event loop. */
        nimble_port_freertos_deinit();
    }

    esp_err_t err = nimble_port_deinit();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit failed: %s", esp_err_to_name(err));
        return err;
    }

    nimble_glue_lock();
    nimble_glue_state.initialized = false;
    nimble_glue_state.synced = false;
    nimble_glue_state.host_started = false;
    nimble_glue_state.random_address_requested = false;
    nimble_glue_state.sync_callback = NULL;
    nimble_glue_state.sync_context = NULL;
    nimble_glue_unlock();
    return ESP_OK;
}

bool nimble_glue_is_initialized(void) {
    nimble_glue_lock();
    bool initialized = nimble_glue_state.initialized;
    nimble_glue_unlock();
    return initialized;
}

bool nimble_glue_is_synced(void) {
    nimble_glue_lock();
    bool synced = nimble_glue_state.synced;
    nimble_glue_unlock();
    return synced;
}

uint8_t nimble_glue_own_address_type(void) {
    nimble_glue_lock();
    uint8_t type = nimble_glue_state.own_address_type;
    nimble_glue_unlock();
    return type;
}

bool nimble_glue_remove_all_bonds(void) {
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int peer_count = 0;
    int rc = ble_store_util_bonded_peers(
        peers, &peer_count, CONFIG_BT_NIMBLE_MAX_BONDS);
    if(rc != 0) {
        ESP_LOGE(TAG, "Failed to enumerate bonds, rc=%d", rc);
        return false;
    }

    bool success = true;
    for(int i = 0; i < peer_count; i++) {
        if(ble_store_util_delete_peer(&peers[i]) != 0) {
            success = false;
        }
    }
    return success;
}
