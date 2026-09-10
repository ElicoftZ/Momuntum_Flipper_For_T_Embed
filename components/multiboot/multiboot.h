#pragma once
#include "layout.h"
#include <esp_err.h>

typedef void (*MbProgress)(uint32_t done, uint32_t total, void* context);
bool multiboot_supported(void);
bool multiboot_plan(uint32_t image_size, MbEntry* added);
uint32_t multiboot_largest_gap(void);
/* These calls run on an internal-RAM stack, spawning a worker only when the
 * caller is not already on one. A successful table change requires a reboot
 * before any further partition or boot API is used. */
esp_err_t multiboot_install(const char* path, uint32_t offset, uint32_t size,
                           MbProgress progress, void* context);
esp_err_t multiboot_remove(uint32_t address);
esp_err_t multiboot_select(uint32_t address);
