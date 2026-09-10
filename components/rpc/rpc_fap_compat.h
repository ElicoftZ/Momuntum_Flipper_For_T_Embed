#pragma once
#include <string.h>
#include "../flipper_application/arm_fap/arm_fap_profile.h"

/* Flipper Lab selects its build using firmware.target and firmware.api.*.
 * Override only RPC device-info fields (legacy and property protocols).
 * Hardware identity, native FAP validation, firmware version and the updater
 * continue to use the real ESP32 values. */
static inline const char* rpc_fap_compat_value(const char* key, const char* value) {
    if(!strcmp(key, "firmware_target") || !strcmp(key, "firmware.target"))
        return ARM_FAP_CATALOG_TARGET;
    if(!strcmp(key, "firmware_api_major") || !strcmp(key, "firmware.api.major"))
        return ARM_FAP_CATALOG_API_MAJOR;
    if(!strcmp(key, "firmware_api_minor") || !strcmp(key, "firmware.api.minor"))
        return ARM_FAP_CATALOG_API_MINOR;
    return value;
}
