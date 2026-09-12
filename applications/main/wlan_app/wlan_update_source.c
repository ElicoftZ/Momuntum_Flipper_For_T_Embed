#include "wlan_update_source.h"

#include <nvs.h>

#define WLAN_UPDATE_SOURCE_NAMESPACE "wlan_update"
#define WLAN_UPDATE_SOURCE_KEY "source"

bool wlan_update_source_get_sor3nt(void) {
    nvs_handle_t handle;
    uint8_t value = 0; // default: Momuntum
    if(nvs_open(WLAN_UPDATE_SOURCE_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, WLAN_UPDATE_SOURCE_KEY, &value);
        nvs_close(handle);
    }
    return value != 0;
}

void wlan_update_source_set_sor3nt(bool use_sor3nt) {
    nvs_handle_t handle;
    if(nvs_open(WLAN_UPDATE_SOURCE_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, WLAN_UPDATE_SOURCE_KEY, use_sor3nt ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
}
