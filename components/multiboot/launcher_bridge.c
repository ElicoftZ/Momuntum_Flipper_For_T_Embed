#include "launcher_bridge.h"
#include <furi.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <nvs.h>
#include <string.h>

static const esp_partition_t* launcher_partition(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* host = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "launcher");
    if(!host || !running || running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
       running->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_15) return NULL;

    nvs_handle_t handle;
    if(nvs_open("flipper_host", NVS_READONLY, &handle) != ESP_OK) return NULL;
    uint32_t address = 0;
    uint8_t protocol = 0;
    uint8_t sha[32];
    size_t length = sizeof(sha);
    bool valid = nvs_get_u8(handle, "protocol", &protocol) == ESP_OK && protocol == 1 &&
                 nvs_get_u32(handle, "address", &address) == ESP_OK && address == host->address &&
                 nvs_get_blob(handle, "sha256", sha, &length) == ESP_OK && length == sizeof(sha);
    nvs_close(handle);
    esp_app_desc_t desc;
    if(!valid || esp_ota_get_partition_description(host, &desc) != ESP_OK ||
       memcmp(sha, desc.app_elf_sha256, sizeof(sha))) return NULL;
    return host;
}

bool launcher_bridge_is_hosted(void) {
    static bool checked;
    static bool hosted;
    if(!checked) {
        hosted = launcher_partition() != NULL;
        checked = true;
    }
    return hosted;
}

static int32_t launcher_return_worker(void* context) {
    esp_err_t* result = context;
    const esp_partition_t* host = launcher_partition();
    if(!host) return *result = ESP_ERR_NOT_FOUND;
    nvs_handle_t handle;
    *result = nvs_open("flipper_host", NVS_READWRITE, &handle);
    if(*result != ESP_OK) return 0;
    *result = nvs_set_u8(handle, "return", 1);
    if(*result == ESP_OK) *result = nvs_commit(handle);
    /* Factory selection validates the image, then clears otadata. It never
     * edits the partition table or writes the running firmware. */
    if(*result == ESP_OK) *result = esp_ota_set_boot_partition(host);
    if(*result != ESP_OK) {
        nvs_erase_key(handle, "return");
        nvs_commit(handle);
    }
    nvs_close(handle);
    if(*result == ESP_OK) esp_restart();
    return 0;
}

esp_err_t launcher_bridge_return(void) {
    esp_err_t result = ESP_FAIL;
    FuriThread* worker = furi_thread_alloc_ex("LauncherReturn", 4096, launcher_return_worker, &result);
    if(!worker) return ESP_ERR_NO_MEM;
    furi_thread_start(worker);
    furi_thread_join(worker);
    furi_thread_free(worker);
    return result;
}
