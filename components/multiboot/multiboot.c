#include "multiboot.h"
#include <furi.h>
#include <storage/storage.h>
#include <esp_flash.h>
#include <esp_flash_encrypt.h>
#include <esp_flash_partitions.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_rom_md5.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#define TAG "MultiBoot"

/* Installing and removing walk the storage/FATFS call chain and need a deep
 * stack; selecting a boot partition only rewrites otadata. Asking a fragmented
 * internal heap for the larger block when the smaller one will do fails the
 * boot with ESP_ERR_NO_MEM. */
#define MB_TABLE_JOB_STACK 8192U
#define MB_SELECT_JOB_STACK 4096U

/* A staged table may already be durable even if its readback failed. Do not
 * reuse its allocated space or select stale OTA indices before reboot. */
static bool reboot_required;

static bool table_valid(const void* buffer, size_t* count) {
    int n = 0;
    if(esp_partition_table_verify(buffer, false, &n) != ESP_OK || n < 0 ||
       !mb_layout_valid(buffer, (size_t)n)) return false;
    *count = (size_t)n;
    return true;
}

/* The preconditions every operation shares, checked without reading the table
 * so the boot path can gate on them for free. */
static bool multiboot_env_ok(void) {
#ifndef CONFIG_MOMENTUM_MULTIBOOT
    return false;
#else
    if(esp_flash_encryption_enabled()) return false;
    const esp_partition_t* running = esp_ota_get_running_partition();
    return running && running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY &&
           running->address == 0x20000;
#endif
}

static uint8_t* load_table(size_t* count) {
    if(!multiboot_env_ok()) return NULL;
    uint8_t* data = heap_caps_malloc(MB_SECTOR_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(data && (esp_flash_read(NULL, data, MB_TABLE_ADDRESS, MB_SECTOR_SIZE) != ESP_OK ||
                !table_valid(data, count))) {
        free(data);
        data = NULL;
    }
    return data;
}

bool multiboot_supported(void) {
    size_t count;
    uint8_t* data = load_table(&count);
    bool valid = data != NULL;
    free(data);
    return valid;
}

bool multiboot_plan(uint32_t image_size, MbEntry* added) {
    size_t count;
    uint8_t* data = load_table(&count);
    bool fits = data && mb_layout_add((MbEntry*)data, &count, image_size, added);
    free(data);
    return fits;
}

uint32_t multiboot_largest_gap(void) {
    size_t count;
    uint8_t* data = load_table(&count);
    uint32_t size = data ? mb_layout_largest_gap((MbEntry*)data, count) : 0;
    free(data);
    return size;
}

static void finish_table(uint8_t* table, size_t count) {
    const size_t bytes = count * sizeof(MbEntry);
    memset(table + bytes, 0xff, MB_SECTOR_SIZE - bytes);
    table[bytes] = 0xeb;
    table[bytes + 1] = 0xeb;
    md5_context_t md5;
    esp_rom_md5_init(&md5);
    esp_rom_md5_update(&md5, table, bytes);
    esp_rom_md5_final(table + bytes + 16, &md5);
}

static esp_err_t save_table(uint8_t* next, size_t count) {
    if(!mb_layout_valid((MbEntry*)next, count)) return ESP_ERR_INVALID_ARG;
    finish_table(next, count);
    uint8_t* check = heap_caps_malloc(MB_SECTOR_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!check) return ESP_ERR_NO_MEM;
    const esp_partition_t* factory = esp_ota_get_running_partition();
    esp_err_t err = esp_ota_set_boot_partition(factory);
    /* Stage the validated table in a data partition. Only the recovery
     * bootloader changes the primary table, so ESP-IDF's protection against
     * writing the bootloader/table/running firmware stays enabled. */
    if(err == ESP_OK) {
        reboot_required = true;
        err = esp_flash_erase_region(NULL, MB_PENDING_ADDRESS, MB_SECTOR_SIZE);
    }
    if(err == ESP_OK) err = esp_flash_write(NULL, next, MB_PENDING_ADDRESS, MB_SECTOR_SIZE);
    if(err == ESP_OK) err = esp_flash_read(NULL, check, MB_PENDING_ADDRESS, MB_SECTOR_SIZE);
    if(err == ESP_OK && memcmp(next, check, MB_SECTOR_SIZE)) err = ESP_FAIL;
    free(check);
    return err;
}

typedef struct {
    const char* path;
    uint32_t offset, size, remove_address, boot_address;
    MbProgress progress;
    void* context;
    esp_err_t result;
    volatile bool done;
} MbJob;

/* Only otadata is rewritten, so the staged table is never needed here. */
static esp_err_t select_boot_partition(uint32_t address) {
    for(unsigned subtype = 0x10; subtype <= 0x1f; ++subtype) {
        const esp_partition_t* part =
            esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, NULL);
        if(part && part->address == address) return esp_ota_set_boot_partition(part);
    }
    return ESP_ERR_NOT_FOUND;
}

static void install_task(void* context) {
    MbJob* job = context;
    size_t count;
    uint8_t* table = NULL;
    uint8_t* buffer = NULL;
    Storage* storage = NULL;
    File* file = NULL;
    job->result = ESP_ERR_INVALID_STATE;
    if(job->boot_address) {
        if(multiboot_env_ok()) job->result = select_boot_partition(job->boot_address);
        goto done;
    }
    table = load_table(&count);
    if(!table) goto done;
    if(job->remove_address) {
        if(mb_layout_remove((MbEntry*)table, &count, job->remove_address))
            job->result = save_table(table, count);
        goto done;
    }
    MbEntry added;
    if(!mb_layout_add((MbEntry*)table, &count, job->size, &added)) {
        job->result = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    buffer = heap_caps_malloc(MB_SECTOR_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!buffer) { job->result = ESP_ERR_NO_MEM; goto done; }
    storage = furi_record_open(RECORD_STORAGE);
    file = storage_file_alloc(storage);
    if(!file || !storage_file_open(file, job->path, FSAM_READ, FSOM_OPEN_EXISTING) ||
       (uint64_t)job->offset + job->size > storage_file_size(file) ||
       !storage_file_seek(file, job->offset, true)) goto done;
    /* Only unallocated space is written. Existing installed images and SD
     * files are never modified, including when a new installation fails. */
    for(uint32_t i = 0; i < added.size; i += MB_ALIGNMENT) {
        job->result = esp_flash_erase_region(NULL, added.offset + i, MB_ALIGNMENT);
        if(job->result != ESP_OK) goto done;
        vTaskDelay(1);
    }
    for(uint32_t i = 0; i < job->size;) {
        uint32_t n = job->size - i;
        if(n > MB_SECTOR_SIZE) n = MB_SECTOR_SIZE;
        if(storage_file_read(file, buffer, n) != n) { job->result = ESP_FAIL; goto done; }
        job->result = esp_flash_write(NULL, buffer, added.offset + i, n);
        if(job->result != ESP_OK) goto done;
        i += n;
        if(job->progress) job->progress(i, job->size, job->context);
    }
    esp_image_metadata_t metadata = {0};
    const esp_partition_pos_t pos = {.offset = added.offset, .size = added.size};
    job->result = esp_image_verify(ESP_IMAGE_VERIFY, &pos, &metadata);
    if(job->result == ESP_OK) job->result = save_table(table, count);
done:
    if(file) { storage_file_close(file); storage_file_free(file); }
    if(storage) furi_record_close(RECORD_STORAGE);
    free(buffer); free(table);
    job->done = true;
    vTaskDelete(NULL);
}

static esp_err_t run_job(MbJob* job, uint32_t stack) {
    if(reboot_required) return ESP_ERR_INVALID_STATE;
    /* xTaskCreate uses internal RAM, unlike FuriThread's PSRAM fallback. */
    if(xTaskCreate(install_task, "MultiBootFlash", stack, job, 5, NULL) != pdPASS) {
        FURI_LOG_E(
            TAG,
            "No %u B internal stack for flash job (%u free, largest block %u)",
            (unsigned)stack,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    while(!job->done) furi_delay_ms(10);
    return job->result;
}

esp_err_t multiboot_install(const char* path, uint32_t offset, uint32_t size,
                           MbProgress progress, void* context) {
    MbJob job = {.path = path, .offset = offset, .size = size, .progress = progress, .context = context};
    return run_job(&job, MB_TABLE_JOB_STACK);
}

esp_err_t multiboot_remove(uint32_t address) {
    MbJob job = {.remove_address = address};
    return run_job(&job, MB_TABLE_JOB_STACK);
}

esp_err_t multiboot_select(uint32_t address) {
    if(reboot_required) return ESP_ERR_INVALID_STATE;
    if(!multiboot_env_ok()) return ESP_ERR_INVALID_STATE;
    /* Rewriting otadata suspends the cache, so the calling stack has to be in
     * internal RAM. A foreground app already owns the internal stack reserve:
     * run there instead of asking a fragmented heap for a worker stack. */
    uint8_t marker;
    if(esp_ptr_internal(&marker)) return select_boot_partition(address);
    MbJob job = {.boot_address = address};
    return run_job(&job, MB_SELECT_JOB_STACK);
}
