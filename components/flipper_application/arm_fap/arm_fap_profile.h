#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Catalog identity for ARM downloads, independent of the native Xtensa ABI.
 * These select packages, not a promise that every exported API is bridged.
 * Keep the whitelist in arm_fap_vm.h and the API audit in sync. */
#define ARM_FAP_CATALOG_TARGET "7"
#define ARM_FAP_CATALOG_API_MAJOR "88"
#define ARM_FAP_CATALOG_API_MINOR "2"

static inline bool arm_fap_profile_accepts(uint16_t major, uint16_t minor) {
    return (major == 87 && minor <= 1) || (major == 88 && minor <= 2);
}
