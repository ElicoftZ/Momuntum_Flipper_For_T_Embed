#include "sdkconfig.h"
#include "layout.h"
#include "bootloader_flash_priv.h"
#include "esp_flash_partitions.h"
#include "esp_log.h"
#include <string.h>

void bootloader_hooks_include(void) {}

#ifdef CONFIG_MOMENTUM_MULTIBOOT
static uint8_t primary[MB_SECTOR_SIZE] __attribute__((aligned(4)));
static uint8_t pending[MB_SECTOR_SIZE] __attribute__((aligned(4)));
static uint8_t backup[MB_SECTOR_SIZE] __attribute__((aligned(4)));

static bool valid(const void* table) {
    int count = 0;
    return esp_partition_table_verify(table, false, &count) == ESP_OK && count > 0 &&
           mb_layout_valid(table, (size_t)count);
}

static bool write_verified(uint32_t address, void* data, void* check) {
    return bootloader_flash_erase_sector(address / MB_SECTOR_SIZE) == ESP_OK &&
           bootloader_flash_write(address, data, MB_SECTOR_SIZE, false) == ESP_OK &&
           bootloader_flash_read(address, check, MB_SECTOR_SIZE, false) == ESP_OK &&
           memcmp(data, check, MB_SECTOR_SIZE) == 0;
}

void bootloader_after_init(void) {
    if(bootloader_flash_read(MB_TABLE_ADDRESS, primary, sizeof(primary), false) != ESP_OK) return;
    if(!valid(primary)) {
        if(bootloader_flash_read(MB_BACKUP_ADDRESS, backup, sizeof(backup), false) != ESP_OK ||
           !valid(backup) || !write_verified(MB_TABLE_ADDRESS, backup, primary)) {
            ESP_LOGE("MultiBoot", "Cannot recover partition table");
            return;
        }
        ESP_LOGW("MultiBoot", "Recovered interrupted partition update");
    }
    if(bootloader_flash_read(MB_PENDING_ADDRESS, pending, sizeof(pending), false) != ESP_OK ||
       !valid(pending)) return;
    if(memcmp(primary, pending, sizeof(primary)) != 0) {
        /* A complete pending table means its app was verified before reboot.
         * Keep old table until the new primary is fully written and verified. */
        if(!write_verified(MB_BACKUP_ADDRESS, primary, backup)) return;
        if(!write_verified(MB_TABLE_ADDRESS, pending, primary)) {
            write_verified(MB_TABLE_ADDRESS, backup, primary);
            return;
        }
        ESP_LOGI("MultiBoot", "Applied new firmware layout");
    }
    bootloader_flash_erase_sector(MB_PENDING_ADDRESS / MB_SECTOR_SIZE);
}
#endif
