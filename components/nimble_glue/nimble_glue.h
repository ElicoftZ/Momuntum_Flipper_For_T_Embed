#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*NimbleGlueSyncCallback)(void* context);

/**
 * Initialize the ESP-IDF NimBLE controller and host.
 *
 * GATT services may be registered after this call and before
 * nimble_glue_start(). Only one firmware feature owns the BLE host at a time.
 */
esp_err_t nimble_glue_init(const char* device_name);

/** Configure the NimBLE security manager before starting the host task. */
void nimble_glue_configure_security(
    bool bonding,
    bool mitm,
    bool secure_connections,
    uint8_t io_capability);

/**
 * Select a static random identity address.
 *
 * The input uses the normal display order (most-significant octet first), as
 * used by the public Flipper BLE profile APIs.
 */
esp_err_t nimble_glue_set_random_address(const uint8_t address[6]);

/** Start the host task and wait until controller/host synchronisation. */
esp_err_t nimble_glue_start(NimbleGlueSyncCallback callback, void* context);

/** Stop and deinitialize the NimBLE host and controller. */
esp_err_t nimble_glue_stop(void);

bool nimble_glue_is_initialized(void);
bool nimble_glue_is_synced(void);
uint8_t nimble_glue_own_address_type(void);

/** Remove every peer bond stored by NimBLE in NVS. */
bool nimble_glue_remove_all_bonds(void);

#ifdef __cplusplus
}
#endif
