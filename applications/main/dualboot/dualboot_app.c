/**
 * @file dualboot_app.c
 * @brief Dual Boot 2.0 manager and recovery for the LilyGo T-Embed CC1101.
 *
 * Momentum remains protected. Each installed firmware gets an aligned app
 * partition in unused flash. SD files are retained; removing an installed app
 * frees only its partition. A recovery bootloader repairs interrupted table
 * updates, and the side key forces a return to factory.
 */

#include <furi.h>
#include <furi_hal.h>

#include <assets_icons.h>
#include <gui/elements.h>
#include <gui/gui.h>
#include <storage/storage.h>
#include <wifi/wlan_hal.h>

#include <esp_app_format.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <multiboot.h>
#include <fw_ota.h>
#include <esp_system.h>
#include <nvs.h>

#include <strings.h>

#define TAG "DualRecovery"

#define DUALBOOT_FW_DIR        EXT_PATH("firmware")
#define DUALBOOT_MAX_FIRMWARES 32
#define DUALBOOT_NAME_LEN      64
#define DUALBOOT_META_LEN      33
#define DUALBOOT_MSG_LEN       96
#define DUALBOOT_CHUNK         4096
#define DUALBOOT_VISIBLE       4
#define DUALBOOT_ONLINE_MAX    32
#define DUALBOOT_ONLINE_NAME   48
#define DUALBOOT_ONLINE_AUTHOR 32
#define DUALBOOT_ONLINE_FID    40
#define DUALBOOT_ONLINE_URL    512
#define DUALBOOT_ONLINE_BODY   (64U * 1024U)
#define DUALBOOT_DOWNLOAD_MAX  (9U * 1024U * 1024U)
#define DUALBOOT_CATALOG_URL                                                        \
    "https://api.launcherhub.net/firmwares?category=t-embed-cc1101&order_by=downloads&page=1"

typedef enum {
    DualBootStateWarning,
    DualBootStateHome,
    DualBootStateBootChoices,
    DualBootStateSecondary,
    DualBootStateFiles,
    DualBootStateOnline,
    DualBootStateConfirmOnline,
    DualBootStateConfirmInstall,
    DualBootStateConfirmDelete,
    DualBootStateWorking,
    DualBootStateDone,
    DualBootStateError,
    DualBootStateHelp,
} DualBootState;

typedef enum {
    DualBootWorkInstall,
    DualBootWorkDelete,
    DualBootWorkCatalog,
    DualBootWorkDownload,
} DualBootWork;

typedef enum {
    DualBootActionBoot,
    DualBootActionInstall,
    DualBootActionOnline,
    DualBootActionDelete,
    DualBootActionRecovery,
} DualBootAction;

typedef enum {
    DualBootEventKey,
    DualBootEventFinished,
} DualBootEventType;

typedef struct {
    DualBootEventType type;
    InputEvent input;
} DualBootEvent;

typedef struct {
    uint64_t offset;
    uint64_t length;
    bool extracted;
    char project[DUALBOOT_META_LEN];
    char version[DUALBOOT_META_LEN];
    uint8_t app_sha[32];
} DualBootImage;

typedef struct {
    bool installed;
    const esp_partition_t* partition;
    char project[DUALBOOT_META_LEN];
    char version[DUALBOOT_META_LEN];
    uint8_t app_sha[32];
} DualBootInstalled;

typedef struct {
    char fid[DUALBOOT_ONLINE_FID];
    char name[DUALBOOT_ONLINE_NAME];
    char author[DUALBOOT_ONLINE_AUTHOR];
} DualBootOnlineItem;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;
    FuriThread* worker;

    DualBootState state;
    DualBootState return_state;
    DualBootState error_return;
    DualBootState install_return;
    DualBootWork work;
    bool running;
    bool quick_mode;
    bool boot_after_install;

    uint8_t home_tab;
    uint8_t menu_idx;
    uint8_t confirm_idx;
    uint8_t done_idx;
    size_t boot_idx;
    size_t boot_top;
    size_t online_count;
    size_t online_idx;
    size_t online_top;

    /* 2 KiB in PSRAM instead of fragmenting internal SRAM. */
    char(*names)[DUALBOOT_NAME_LEN];
    DualBootOnlineItem* online;
    char* online_url;
    size_t count;
    size_t idx;
    size_t top;

    DualBootInstalled installed;
    DualBootInstalled slots[MB_MAX_APPS];
    uint8_t installed_count;
    uint8_t selected_slot;
    uint8_t home_top;
    DualBootImage pending;
    float progress;
    char online_version[24];
    char download_name[DUALBOOT_NAME_LEN];
    char message[DUALBOOT_MSG_LEN];
} DualBoot;

/* -------------------------------------------------------------------------- */
/* Partitions and metadata                                                     */
/* -------------------------------------------------------------------------- */

static const esp_partition_t* dualboot_target_partition(const DualBoot* app) {
    return app->selected_slot < app->installed_count ? app->slots[app->selected_slot].partition : NULL;
}

static void dualboot_copy_metadata(char* out, const char* in, size_t in_size) {
    size_t length = strnlen(in, in_size);
    if(length >= DUALBOOT_META_LEN) length = DUALBOOT_META_LEN - 1;
    memcpy(out, in, length);
    out[length] = '\0';
    if(length == 0) strlcpy(out, "Unknown", DUALBOOT_META_LEN);
}

#define DUALBOOT_NVS_NAMESPACE "dualboot"

static bool dualboot_sha_is_set(const uint8_t sha[32]) {
    for(size_t i = 0; i < 32; ++i) {
        if(sha[i] != 0) return true;
    }
    return false;
}

static void dualboot_friendly_from_filename(
    char out[DUALBOOT_META_LEN],
    const char* filename) {
    const char* dot = strrchr(filename, '.');
    size_t length = dot && strcasecmp(dot, ".bin") == 0 ? (size_t)(dot - filename) :
                                                         strlen(filename);
    if(length >= DUALBOOT_META_LEN) length = DUALBOOT_META_LEN - 1;
    memcpy(out, filename, length);
    out[length] = '\0';
    for(size_t i = 0; i < length; ++i) {
        if(out[i] == '_') out[i] = ' ';
    }
}

static bool dualboot_load_installed_name(char out[DUALBOOT_META_LEN], const uint8_t sha[32], uint32_t address) {
    if(!dualboot_sha_is_set(sha)) return false;
    nvs_handle_t handle;
    if(nvs_open(DUALBOOT_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    char sha_key[16], name_key[16];
    snprintf(sha_key, sizeof(sha_key), "s%08lx", (unsigned long)address);
    snprintf(name_key, sizeof(name_key), "n%08lx", (unsigned long)address);
    uint8_t saved_sha[32];
    size_t sha_size = sizeof(saved_sha);
    size_t name_size = DUALBOOT_META_LEN;
    const bool valid = nvs_get_blob(handle, sha_key, saved_sha, &sha_size) == ESP_OK &&
                       sha_size == sizeof(saved_sha) &&
                       memcmp(saved_sha, sha, sizeof(saved_sha)) == 0 &&
                       nvs_get_str(handle, name_key, out, &name_size) == ESP_OK && out[0];
    nvs_close(handle);
    return valid;
}

static void dualboot_store_installed_name(const char* filename, const uint8_t sha[32], uint32_t address) {
    if(!dualboot_sha_is_set(sha)) return;
    char friendly[DUALBOOT_META_LEN];
    dualboot_friendly_from_filename(friendly, filename);
    if(!friendly[0]) return;

    nvs_handle_t handle;
    if(nvs_open(DUALBOOT_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    char sha_key[16], name_key[16];
    snprintf(sha_key, sizeof(sha_key), "s%08lx", (unsigned long)address);
    snprintf(name_key, sizeof(name_key), "n%08lx", (unsigned long)address);
    if(nvs_set_blob(handle, sha_key, sha, 32) == ESP_OK &&
       nvs_set_str(handle, name_key, friendly) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

static void dualboot_clear_installed_name(uint32_t address) {
    nvs_handle_t handle;
    if(nvs_open(DUALBOOT_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    char key[16];
    snprintf(key, sizeof(key), "s%08lx", (unsigned long)address);
    nvs_erase_key(handle, key);
    snprintf(key, sizeof(key), "n%08lx", (unsigned long)address);
    nvs_erase_key(handle, key);
    nvs_commit(handle);
    nvs_close(handle);
}

static void dualboot_refresh_installed(DualBoot* app) {
    app->installed_count = 0;
    for(unsigned subtype = 0x10; subtype <= 0x1f; ++subtype) {
        const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, NULL);
        /* Below the pool sits the fixed otaupd slot (ota_0), which is updater
         * scratch rather than an installed firmware - never a boot tile. */
        if(!part || part->address < MB_POOL_START || part == esp_ota_get_running_partition())
            continue;
        DualBootInstalled* slot = &app->slots[app->installed_count++];
        memset(slot, 0, sizeof(*slot));
        slot->partition = part;
        slot->installed = true;
        esp_app_desc_t description;
        if(esp_ota_get_partition_description(part, &description) == ESP_OK) {
            dualboot_copy_metadata(slot->project, description.project_name, sizeof(description.project_name));
            dualboot_copy_metadata(slot->version, description.version, sizeof(description.version));
            memcpy(slot->app_sha, description.app_elf_sha256, sizeof(slot->app_sha));
            char friendly[DUALBOOT_META_LEN];
            if(dualboot_load_installed_name(friendly, slot->app_sha, part->address))
                strlcpy(slot->project, friendly, sizeof(slot->project));
        } else {
            strlcpy(slot->project, "Invalid image", sizeof(slot->project));
        }
    }
    if(app->selected_slot >= app->installed_count) app->selected_slot = 0;
    memset(&app->installed, 0, sizeof(app->installed));
    if(app->installed_count) app->installed = app->slots[app->selected_slot];
}

static esp_err_t dualboot_select_secondary(DualBoot* app) {
    const esp_partition_t* target = dualboot_target_partition(app);
    return target ? multiboot_select(target->address) : ESP_ERR_NOT_FOUND;
}

static const char* dualboot_locate_image(File* file, uint64_t size, DualBootImage* out);

/** Keep corrupt, interrupted, and wrong-device files out of the boot tiles. */
static bool dualboot_path_inspect(
    Storage* storage,
    const char* path,
    DualBootImage* inspected) {
    File* file = storage_file_alloc(storage);
    bool valid = false;
    DualBootImage local;
    DualBootImage* image = inspected ? inspected : &local;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        valid = !dualboot_locate_image(file, storage_file_size(file), image) &&
                image->length <= MB_POOL_END - MB_POOL_START;
    }
    storage_file_close(file);
    storage_file_free(file);
    return valid;
}

/* -------------------------------------------------------------------------- */
/* SD firmware catalogue                                                       */
/* -------------------------------------------------------------------------- */

static bool dualboot_name_is_bin(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".bin") == 0;
}

static void dualboot_sort_names(DualBoot* app) {
    for(size_t i = 1; i < app->count; ++i) {
        char current[DUALBOOT_NAME_LEN];
        strlcpy(current, app->names[i], sizeof(current));
        size_t j = i;
        while(j > 0 && strcasecmp(app->names[j - 1], current) > 0) {
            strlcpy(app->names[j], app->names[j - 1], DUALBOOT_NAME_LEN);
            --j;
        }
        strlcpy(app->names[j], current, DUALBOOT_NAME_LEN);
    }
}

static bool dualboot_scan(DualBoot* app, Storage* storage) {
    app->count = 0;
    app->idx = 0;
    app->top = 0;
    app->boot_idx = 0;
    app->boot_top = 0;

    File* dir = storage_file_alloc(storage);
    const bool opened = storage_dir_open(dir, DUALBOOT_FW_DIR);
    if(opened) {
        FileInfo info;
        char name[DUALBOOT_NAME_LEN];
        while(app->count < DUALBOOT_MAX_FIRMWARES &&
              storage_dir_read(dir, &info, name, sizeof(name))) {
            if(file_info_is_dir(&info) || !dualboot_name_is_bin(name)) continue;
            char path[sizeof(DUALBOOT_FW_DIR) + DUALBOOT_NAME_LEN + 2];
            snprintf(path, sizeof(path), "%s/%s", DUALBOOT_FW_DIR, name);
            DualBootImage image;
            if(!dualboot_path_inspect(storage, path, &image)) {
                FURI_LOG_W(TAG, "Ignoring invalid firmware file: %s", name);
                continue;
            }
            bool already_installed = false;
            for(unsigned slot_index = 0; slot_index < app->installed_count; ++slot_index) {
                DualBootInstalled* slot = &app->slots[slot_index];
                if(dualboot_sha_is_set(image.app_sha) &&
                   memcmp(image.app_sha, slot->app_sha, sizeof(image.app_sha)) == 0) {
                    already_installed = true;
                    char friendly[DUALBOOT_META_LEN];
                    dualboot_friendly_from_filename(friendly, name);
                    if(strcmp(friendly, slot->project) != 0) {
                        strlcpy(slot->project, friendly, sizeof(slot->project));
                        dualboot_store_installed_name(name, image.app_sha, slot->partition->address);
                    }
                }
            }
            /* Keep the SD copy, but route this image through its installed
             * Boot entry instead of offering to install it a second time. */
            if(!already_installed) strlcpy(app->names[app->count++], name, DUALBOOT_NAME_LEN);
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    dualboot_sort_names(app);
    return opened;
}

static void dualboot_scan_or_create(DualBoot* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!dualboot_scan(app, storage) && !storage_simply_mkdir(storage, DUALBOOT_FW_DIR)) {
        FURI_LOG_W(TAG, "Cannot create %s", DUALBOOT_FW_DIR);
    }
    furi_record_close(RECORD_STORAGE);
}

static size_t dualboot_boot_choice_count(const DualBoot* app) {
    return app->count + app->installed_count;
}

static bool dualboot_choice_is_installed(const DualBoot* app, size_t choice) {
    return choice < app->installed_count;
}

static size_t dualboot_choice_file_index(const DualBoot* app, size_t choice) {
    return choice - app->installed_count;
}

/* -------------------------------------------------------------------------- */
/* File validation                                                             */
/* -------------------------------------------------------------------------- */

#define DUALBOOT_PART_TABLE_OFFSET 0x8000
#define DUALBOOT_PART_TABLE_MAGIC  0x50AAU
#define DUALBOOT_PART_TYPE_APP     0x00
#define DUALBOOT_PART_ENTRY_MAX    95

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t type;
    uint8_t subtype;
    uint32_t offset;
    uint32_t size;
    uint8_t label[16];
    uint32_t flags;
} DualBootPartEntry;

static void dualboot_read_app_description(File* file, uint64_t offset, DualBootImage* out) {
    esp_app_desc_t description;
    const uint64_t location =
        offset + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if(location <= UINT32_MAX && storage_file_seek(file, (uint32_t)location, true) &&
       storage_file_read(file, &description, sizeof(description)) == sizeof(description) &&
       description.magic_word == ESP_APP_DESC_MAGIC_WORD) {
        dualboot_copy_metadata(out->project, description.project_name, sizeof(description.project_name));
        dualboot_copy_metadata(out->version, description.version, sizeof(description.version));
        memcpy(out->app_sha, description.app_elf_sha256, sizeof(out->app_sha));
    } else {
        strlcpy(out->project, "Unknown", sizeof(out->project));
        strlcpy(out->version, "Unknown", sizeof(out->version));
    }
}

/** Accept either a raw app or the first app embedded in a merged flash image. */
static const char* dualboot_locate_image(File* file, uint64_t size, DualBootImage* out) {
    esp_image_header_t header;
    memset(out, 0, sizeof(*out));
    out->length = size;

    if(!storage_file_seek(file, 0, true)) return "Cannot read file";
    if(storage_file_read(file, &header, sizeof(header)) != sizeof(header)) return "File too small";
    if(header.magic != ESP_IMAGE_HEADER_MAGIC) return "Not a firmware image";

    if(size > DUALBOOT_PART_TABLE_OFFSET + sizeof(DualBootPartEntry) &&
       storage_file_seek(file, DUALBOOT_PART_TABLE_OFFSET, true)) {
        for(size_t i = 0; i < DUALBOOT_PART_ENTRY_MAX; ++i) {
            DualBootPartEntry entry;
            if(storage_file_read(file, &entry, sizeof(entry)) != sizeof(entry)) break;
            if(entry.magic != DUALBOOT_PART_TABLE_MAGIC) break;
            if(entry.type != DUALBOOT_PART_TYPE_APP || entry.offset >= size) continue;
            out->offset = entry.offset;
            out->length = size - entry.offset;
            if(out->length > entry.size) out->length = entry.size;
            out->extracted = true;
            break;
        }
    }

    if(out->offset > UINT32_MAX || !storage_file_seek(file, (uint32_t)out->offset, true)) {
        return "Cannot read file";
    }
    if(storage_file_read(file, &header, sizeof(header)) != sizeof(header)) return "Truncated image";
    if(header.magic != ESP_IMAGE_HEADER_MAGIC) return "No app inside image";
    if(header.chip_id != ESP_CHIP_ID_ESP32S3) return "Wrong device: not ESP32-S3";
    if(header.segment_count == 0 || header.segment_count > 16) return "Corrupt image header";

    /* Derive the app's actual encoded length. Merged images may contain
     * padding and filesystem data after the app; none belongs in its slot. */
    uint64_t end = out->offset + sizeof(header);
    for(unsigned i = 0; i < header.segment_count; ++i) {
        esp_image_segment_header_t segment;
        if(end > UINT32_MAX || end + sizeof(segment) > size ||
           !storage_file_seek(file, (uint32_t)end, true) ||
           storage_file_read(file, &segment, sizeof(segment)) != sizeof(segment))
            return "Truncated segment header";
        end += sizeof(segment) + (uint64_t)segment.data_len;
        if(end > size) return "Truncated segment data";
    }
    end = out->offset + ((end - out->offset + 16U) & ~(uint64_t)15U);
    if(header.hash_appended) end += 32;
    if(end > size || end - out->offset > MB_POOL_END - MB_POOL_START)
        return "Truncated / oversized app";
    out->length = end - out->offset;

    dualboot_read_app_description(file, out->offset, out);
    if(!storage_file_seek(file, (uint32_t)out->offset, true)) return "Cannot read file";
    return NULL;
}

static void dualboot_set_error(
    DualBoot* app,
    const char* message,
    DualBootState return_state) {
    strlcpy(app->message, message, sizeof(app->message));
    app->error_return = return_state;
    app->state = DualBootStateError;
}

static void dualboot_fail_worker(DualBoot* app, const char* message) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    dualboot_set_error(
        app,
        message,
        app->quick_mode ? DualBootStateBootChoices : DualBootStateSecondary);
    furi_mutex_release(app->mutex);
}

static bool
    dualboot_prepare_path(DualBoot* app, const char* path, DualBootState error_return) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool valid = false;

    do {
        if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            dualboot_set_error(app, "Cannot open file", error_return);
            break;
        }
        const char* reason = dualboot_locate_image(file, storage_file_size(file), &app->pending);
        if(reason) {
            dualboot_set_error(app, reason, error_return);
            break;
        }
        MbEntry planned;
        if(app->pending.length > UINT32_MAX || !multiboot_plan((uint32_t)app->pending.length, &planned)) {
            dualboot_set_error(app, "No free slot / flash space", error_return);
            break;
        }
        valid = true;
    } while(false);

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return valid;
}

static bool dualboot_prepare_file(DualBoot* app, size_t file_index, DualBootState error_return) {
    if(file_index >= app->count) return false;
    app->idx = file_index;

    char path[sizeof(DUALBOOT_FW_DIR) + DUALBOOT_NAME_LEN + 2];
    snprintf(path, sizeof(path), "%s/%s", DUALBOOT_FW_DIR, app->names[app->idx]);
    return dualboot_prepare_path(app, path, error_return);
}

/* -------------------------------------------------------------------------- */
/* T-Embed-only online catalogue                                               */
/* -------------------------------------------------------------------------- */

static bool dualboot_contains_case(const char* text, const char* needle) {
    const size_t needle_length = strlen(needle);
    if(needle_length == 0) return true;
    for(; *text; ++text) {
        if(strncasecmp(text, needle, needle_length) == 0) return true;
    }
    return false;
}

static bool dualboot_online_item_allowed(const char* name) {
    /* LauncherHub's category currently contains two explicitly different
     * hardware variants. Never offer those on the original CC1101 board. */
    return !dualboot_contains_case(name, "not cc1101") &&
           !dualboot_contains_case(name, "(plus)") &&
           !dualboot_contains_case(name, "bboink");
}

static bool dualboot_json_string(
    const char* begin,
    const char* limit,
    const char* key,
    char* out,
    size_t out_size,
    const char** after) {
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* found = strstr(begin, pattern);
    if(!found || (limit && found >= limit)) return false;
    found += strlen(pattern);
    while(*found == ' ' || *found == '\t') ++found;
    if(*found++ != ':') return false;
    while(*found == ' ' || *found == '\t') ++found;
    if(*found++ != '\"') return false;

    size_t length = 0;
    while(*found && *found != '\"' && (!limit || found < limit)) {
        char c = *found++;
        if(c == '\\' && *found) {
            const char escaped = *found++;
            if(escaped == '/' || escaped == '\\' || escaped == '\"') c = escaped;
            else if(escaped == 'n') c = '\n';
            else if(escaped == 'r') c = '\r';
            else if(escaped == 't') c = '\t';
            else c = escaped;
        }
        if(length + 1 < out_size) out[length++] = c;
    }
    if(*found != '\"') return false;
    out[length] = '\0';
    if(after) *after = found + 1;
    return length > 0;
}

static char* dualboot_http_get_text(const char* url, size_t maximum, size_t* out_length) {
    char* body = heap_caps_malloc(maximum + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!body) return NULL;

    esp_http_client_config_t config = {
        .url = url,
        .user_agent = "Momentum-DualRecovery/2.0",
        .timeout_ms = 30000,
        .buffer_size = DUALBOOT_CHUNK,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if(!client) {
        heap_caps_free(body);
        return NULL;
    }

    size_t length = 0;
    bool success = false;
    if(esp_http_client_open(client, 0) == ESP_OK) {
        const int64_t declared = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if(status == 200 && (declared < 0 || declared <= (int64_t)maximum)) {
            while(length < maximum) {
                const int got =
                    esp_http_client_read(client, body + length, maximum - length);
                if(got < 0) {
                    length = 0;
                    break;
                }
                if(got == 0) {
                    success = esp_http_client_is_complete_data_received(client);
                    break;
                }
                length += (size_t)got;
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if(!success || length == 0) {
        heap_caps_free(body);
        return NULL;
    }
    body[length] = '\0';
    if(out_length) *out_length = length;
    return body;
}

static bool dualboot_online_connect(DualBoot* app, DualBootState error_return) {
    if(wlan_hal_is_connected()) return true;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    strlcpy(app->message, "Enabling WiFi...", sizeof(app->message));
    app->progress = 0.05f;
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);

    if(!wlan_hal_set_user_enabled(true)) {
        dualboot_fail_worker(app, "WiFi could not start");
        return false;
    }
    for(size_t attempt = 0; attempt < 120 && !wlan_hal_is_connected(); ++attempt) {
        furi_delay_ms(100);
    }
    if(!wlan_hal_is_connected()) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        dualboot_set_error(app, "Connect WiFi in WiFi app", error_return);
        furi_mutex_release(app->mutex);
        return false;
    }
    return true;
}

static bool dualboot_worker_catalog(DualBoot* app) {
    if(!dualboot_online_connect(app, DualBootStateSecondary)) return false;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    strlcpy(app->message, "Searching T-Embed...", sizeof(app->message));
    app->progress = 0.25f;
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);

    size_t body_length = 0;
    char* body = dualboot_http_get_text(DUALBOOT_CATALOG_URL, 16U * 1024U, &body_length);
    if(!body) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        dualboot_set_error(app, "Online catalogue failed", DualBootStateSecondary);
        furi_mutex_release(app->mutex);
        return false;
    }

    size_t count = 0;
    const char* cursor = body;
    const char* end = body + body_length;
    while(count < DUALBOOT_ONLINE_MAX && cursor < end) {
        const char* object = strstr(cursor, "{\"fid\"");
        if(!object || object >= end) break;
        const char* object_end = strchr(object, '}');
        if(!object_end || object_end > end) break;

        DualBootOnlineItem item = {0};
        if(dualboot_json_string(
               object, object_end, "fid", item.fid, sizeof(item.fid), NULL) &&
           dualboot_json_string(
               object, object_end, "name", item.name, sizeof(item.name), NULL) &&
           dualboot_json_string(
               object, object_end, "author", item.author, sizeof(item.author), NULL) &&
           dualboot_online_item_allowed(item.name)) {
            app->online[count++] = item;
        }
        cursor = object_end + 1;
    }
    heap_caps_free(body);

    if(count == 0) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        dualboot_set_error(app, "No compatible firmware found", DualBootStateSecondary);
        furi_mutex_release(app->mutex);
        return false;
    }

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->online_count = count;
    app->online_idx = 0;
    app->online_top = 0;
    app->progress = 1.0f;
    app->state = DualBootStateOnline;
    snprintf(app->message, sizeof(app->message), "%u compatible", (unsigned)count);
    furi_mutex_release(app->mutex);
    return true;
}

typedef struct {
    DualBoot* app;
    File* file;
    size_t received;
    size_t expected;
    bool write_failed;
} DualBootDownload;

static esp_err_t dualboot_download_event(esp_http_client_event_t* event) {
    DualBootDownload* download = event->user_data;
    if(!download) return ESP_OK;
    if(event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        /* Redirect responses may carry a body.  Only the final 200 response is firmware. */
        if(esp_http_client_get_status_code(event->client) != 200) return ESP_OK;
        const size_t length = (size_t)event->data_len;
        if(download->received + length > DUALBOOT_DOWNLOAD_MAX ||
           storage_file_write(download->file, event->data, length) != length) {
            download->write_failed = true;
            return ESP_FAIL;
        }
        download->received += length;
        if(download->expected == 0) {
            const int64_t declared = esp_http_client_get_content_length(event->client);
            if(declared > 0) download->expected = (size_t)declared;
        }

        furi_mutex_acquire(download->app->mutex, FuriWaitForever);
        download->app->progress = download->expected ?
                                      (float)((double)download->received /
                                              (double)download->expected) :
                                      0.35f;
        snprintf(
            download->app->message,
            sizeof(download->app->message),
            "%lu / %lu KiB",
            (unsigned long)(download->received / 1024),
            (unsigned long)(download->expected / 1024));
        furi_mutex_release(download->app->mutex);
        view_port_update(download->app->view_port);
    }
    return ESP_OK;
}

static void dualboot_make_download_name(DualBoot* app, const DualBootOnlineItem* item) {
    snprintf(
        app->download_name,
        sizeof(app->download_name),
        "%.36s-%.14s.bin",
        item->name,
        app->online_version);
    for(char* c = app->download_name; *c; ++c) {
        if((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
           (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.') {
            continue;
        }
        *c = '_';
    }
}

static bool dualboot_worker_download(DualBoot* app) {
    if(app->online_idx >= app->online_count) return false;
    if(!dualboot_online_connect(app, DualBootStateOnline)) return false;

    const DualBootOnlineItem item = app->online[app->online_idx];
    char detail_url[128];
    snprintf(
        detail_url,
        sizeof(detail_url),
        "https://api.launcherhub.net/firmwares?fid=%s",
        item.fid);
    size_t body_length = 0;
    char* body = dualboot_http_get_text(detail_url, DUALBOOT_ONLINE_BODY, &body_length);
    if(!body) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        dualboot_set_error(app, "Firmware details failed", DualBootStateOnline);
        furi_mutex_release(app->mutex);
        return false;
    }

    const char* versions = strstr(body, "\"versions\"");
    const bool detail_ok = versions &&
                           dualboot_json_string(
                               versions,
                               body + body_length,
                               "version",
                               app->online_version,
                               sizeof(app->online_version),
                               NULL) &&
                           dualboot_json_string(
                               versions,
                               body + body_length,
                               "file",
                               app->online_url,
                               DUALBOOT_ONLINE_URL,
                               NULL) &&
                           strncmp(app->online_url, "https://", 8) == 0;
    heap_caps_free(body);
    if(!detail_ok) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        dualboot_set_error(app, "Bad catalogue details", DualBootStateOnline);
        furi_mutex_release(app->mutex);
        return false;
    }

    dualboot_make_download_name(app, &item);
    char path[sizeof(DUALBOOT_FW_DIR) + DUALBOOT_NAME_LEN + 2];
    char temp_path[sizeof(DUALBOOT_FW_DIR) + DUALBOOT_NAME_LEN + 7];
    snprintf(path, sizeof(path), "%s/%s", DUALBOOT_FW_DIR, app->download_name);
    snprintf(temp_path, sizeof(temp_path), "%s.part", path);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, DUALBOOT_FW_DIR);
    /* A reset or power loss may leave this staging file, but it never has a
     * .bin extension and therefore can never become a boot choice. */
    storage_common_remove(storage, temp_path);
    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, temp_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        dualboot_set_error(app, "Cannot create SD download", DualBootStateOnline);
        furi_mutex_release(app->mutex);
        return false;
    }

    DualBootDownload download = {.app = app, .file = file};
    esp_http_client_config_t config = {
        .url = app->online_url,
        .user_agent = "Momentum-DualRecovery/2.0",
        .event_handler = dualboot_download_event,
        .user_data = &download,
        .timeout_ms = 40000,
        .buffer_size = DUALBOOT_CHUNK,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t http_error = client ? esp_http_client_perform(client) : ESP_ERR_NO_MEM;
    const int status = client ? esp_http_client_get_status_code(client) : 0;
    const int64_t declared = client ? esp_http_client_get_content_length(client) : -1;
    if(client) esp_http_client_cleanup(client);
    const bool synced = storage_file_sync(file);
    storage_file_close(file);
    storage_file_free(file);

    bool complete = http_error == ESP_OK && status == 200 && synced && !download.write_failed &&
                    download.received > 0 && download.received <= DUALBOOT_DOWNLOAD_MAX &&
                    (declared < 0 || download.received == (size_t)declared);
    if(!complete) {
        storage_common_remove(storage, temp_path);
        furi_record_close(RECORD_STORAGE);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        dualboot_set_error(app, "Download failed or incomplete", DualBootStateOnline);
        furi_mutex_release(app->mutex);
        FURI_LOG_E(
            TAG,
            "Download failed: err=%s status=%d bytes=%u",
            esp_err_to_name(http_error),
            status,
            (unsigned)download.received);
        return false;
    }

    /* Validate the staged bytes before their name becomes visible to the boot
     * catalogue, then publish them with a single filesystem rename. */
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    const bool prepared = dualboot_prepare_path(app, temp_path, DualBootStateOnline);
    furi_mutex_release(app->mutex);
    if(!prepared) {
        storage_common_remove(storage, temp_path);
        furi_record_close(RECORD_STORAGE);
        if(app->state != DualBootStateError) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            dualboot_set_error(app, "Downloaded image is invalid", DualBootStateOnline);
            furi_mutex_release(app->mutex);
        }
        return false;
    }

    FS_Error rename_error = storage_common_rename(storage, temp_path, path);
    if(rename_error == FSE_EXIST) {
        storage_common_remove(storage, path);
        rename_error = storage_common_rename(storage, temp_path, path);
    }
    if(rename_error != FSE_OK) {
        storage_common_remove(storage, temp_path);
        furi_record_close(RECORD_STORAGE);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        dualboot_set_error(app, "Cannot publish SD firmware", DualBootStateOnline);
        furi_mutex_release(app->mutex);
        return false;
    }
    furi_record_close(RECORD_STORAGE);

    dualboot_scan_or_create(app);
    size_t file_index = app->count;
    for(size_t i = 0; i < app->count; ++i) {
        if(strcmp(app->names[i], app->download_name) == 0) {
            file_index = i;
            break;
        }
    }
    if(file_index >= app->count) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        dualboot_set_error(app, "SD firmware disappeared", DualBootStateOnline);
        furi_mutex_release(app->mutex);
        return false;
    }
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->idx = file_index;
    app->confirm_idx = 0;
    app->boot_after_install = false;
    app->install_return = DualBootStateOnline;
    app->state = DualBootStateConfirmInstall;
    furi_mutex_release(app->mutex);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Install/delete/online worker                                                */
/* -------------------------------------------------------------------------- */

static void dualboot_install_progress(uint32_t done, uint32_t total, void* context) {
    DualBoot* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->progress = total ? (float)done / total : 0;
    snprintf(app->message, sizeof(app->message), "%lu / %lu KiB",
             (unsigned long)(done / 1024), (unsigned long)(total / 1024));
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
}

static bool dualboot_worker_install(DualBoot* app) {
    char path[sizeof(DUALBOOT_FW_DIR) + DUALBOOT_NAME_LEN + 2];
    snprintf(path, sizeof(path), "%s/%s", DUALBOOT_FW_DIR, app->names[app->idx]);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    DualBootImage image;
    bool valid = dualboot_path_inspect(storage, path, &image);
    furi_record_close(RECORD_STORAGE);
    MbEntry planned;
    if(!valid || image.length > UINT32_MAX || !multiboot_plan((uint32_t)image.length, &planned)) {
        dualboot_fail_worker(app, "Invalid image / no space");
        return false;
    }
    esp_err_t err = multiboot_install(path, image.offset, image.length, dualboot_install_progress, app);
    if(err != ESP_OK) { dualboot_fail_worker(app, esp_err_to_name(err)); return false; }
    dualboot_store_installed_name(app->names[app->idx], image.app_sha, planned.offset);
    return true;
}

static bool dualboot_worker_delete(DualBoot* app) {
    const esp_partition_t* target = dualboot_target_partition(app);
    if(!target) { dualboot_fail_worker(app, "No selected firmware"); return false; }
    esp_err_t err = multiboot_remove(target->address);
    if(err != ESP_OK) { dualboot_fail_worker(app, esp_err_to_name(err)); return false; }
    dualboot_clear_installed_name(target->address);
    return true;
}

static int32_t dualboot_worker(void* context) {
    DualBoot* app = context;
    bool success = false;
    switch(app->work) {
    case DualBootWorkInstall:
        success = dualboot_worker_install(app);
        break;
    case DualBootWorkDelete:
        success = dualboot_worker_delete(app);
        break;
    case DualBootWorkCatalog:
        success = dualboot_worker_catalog(app);
        break;
    case DualBootWorkDownload:
        success = dualboot_worker_download(app);
        break;
    }

    if(success &&
       (app->work == DualBootWorkInstall || app->work == DualBootWorkDelete)) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->progress = 1.0f;
        app->done_idx = 0;
        app->state = DualBootStateDone;
        strlcpy(
            app->message,
            app->work == DualBootWorkInstall ? "Installed and verified" : "Firmware removed",
            sizeof(app->message));
        furi_mutex_release(app->mutex);
    }

    const DualBootEvent event = {.type = DualBootEventFinished};
    furi_message_queue_put(app->queue, &event, FuriWaitForever);
    return 0;
}

static void dualboot_start_worker(DualBoot* app, DualBootWork work, bool boot_after) {
    app->work = work;
    app->boot_after_install = boot_after;
    app->progress = 0.0f;
    const char* message = "Preparing...";
    if(work == DualBootWorkInstall) message = "Preparing image...";
    else if(work == DualBootWorkDelete) message = "Preparing erase...";
    else if(work == DualBootWorkCatalog) message = "Starting online search...";
    else if(work == DualBootWorkDownload) message = "Getting latest version...";
    strlcpy(app->message, message, sizeof(app->message));
    app->state = DualBootStateWorking;
    /* HTTPS/TLS needs more temporary stack than an SD-only install. All
     * catalogue/results buffers remain explicitly in PSRAM. */
    app->worker = furi_thread_alloc_ex("DualRecovery", 8192, dualboot_worker, app);
    furi_thread_start(app->worker);
}

/* -------------------------------------------------------------------------- */
/* Rendering                                                                   */
/* -------------------------------------------------------------------------- */

static void dualboot_short_text(char* out, size_t out_size, const char* in, size_t max_chars) {
    strlcpy(out, in ? in : "", out_size);
    if(strlen(out) > max_chars && max_chars >= 3) {
        out[max_chars - 2] = '.';
        out[max_chars - 1] = '.';
        out[max_chars] = '\0';
    }
}

static void dualboot_render_home(Canvas* canvas, DualBoot* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignCenter, "Dual Boot: installed");
    canvas_set_font(canvas, FontSecondary);
    for(unsigned row = 0; row < DUALBOOT_VISIBLE; ++row) {
        unsigned index = app->home_top + row;
        if(index >= app->installed_count + 3U) break;
        const char* text = index == 0 ? "Momentum (protected)" :
            index <= app->installed_count ? app->slots[index - 1].project :
            index == app->installed_count + 1U ? "+ Install from SD" : "+ Online search";
        char label[25];
        if(index > 0 && index <= app->installed_count)
            snprintf(label, sizeof(label), "Boot %.19s", text);
        else
            dualboot_short_text(label, sizeof(label), text, 21);
        uint8_t y = 19 + row * 12;
        canvas_draw_str_aligned(canvas, 64, y, AlignCenter, AlignCenter, label);
        if(index == app->home_tab) elements_frame(canvas, 2, y - 6, 124, 12);
    }
}

static void dualboot_choice_label(
    const DualBoot* app,
    size_t choice,
    char* out,
    size_t out_size) {
    if(dualboot_choice_is_installed(app, choice)) {
        snprintf(out, out_size, "Boot %s", app->slots[choice].project);
        return;
    }
    const size_t file_index = dualboot_choice_file_index(app, choice);
    dualboot_short_text(out, out_size, app->names[file_index], 20);
    char* dot = strrchr(out, '.');
    if(dot && strcasecmp(dot, ".bin") == 0) *dot = '\0';
}

static void dualboot_render_boot_choices(Canvas* canvas, DualBoot* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignCenter, "What do you want to boot?");
    canvas_set_font(canvas, FontSecondary);
    const size_t count = dualboot_boot_choice_count(app);
    if(count == 0) {
        canvas_draw_str_aligned(canvas, 64, 31, AlignCenter, AlignCenter, "No firmware found");
        canvas_draw_str_aligned(canvas, 64, 47, AlignCenter, AlignCenter, "Use Apps > Dual Boot");
        return;
    }

    for(size_t row = 0; row < DUALBOOT_VISIBLE; ++row) {
        const size_t choice = app->boot_top + row;
        if(choice >= count) break;
        char label[28];
        dualboot_choice_label(app, choice, label, sizeof(label));
        const uint8_t y = 19 + (uint8_t)(row * 12);
        canvas_draw_str_aligned(canvas, 64, y, AlignCenter, AlignCenter, label);
        if(choice == app->boot_idx) elements_frame(canvas, 2, y - 6, 124, 12);
    }
}

static uint8_t dualboot_action_count(const DualBoot* app) {
    return app->installed.installed ? 5 : 3;
}

static DualBootAction dualboot_action_at(const DualBoot* app, uint8_t index) {
    if(app->installed.installed) {
        static const DualBootAction actions[] = {
            DualBootActionBoot,
            DualBootActionInstall,
            DualBootActionOnline,
            DualBootActionDelete,
            DualBootActionRecovery,
        };
        return actions[index < COUNT_OF(actions) ? index : 0];
    }
    static const DualBootAction empty_actions[] = {
        DualBootActionInstall,
        DualBootActionOnline,
        DualBootActionRecovery,
    };
    return empty_actions[index < COUNT_OF(empty_actions) ? index : 0];
}

static const char* dualboot_action_label(const DualBoot* app, DualBootAction action) {
    switch(action) {
    case DualBootActionBoot:
        return "Boot firmware";
    case DualBootActionInstall:
        return app->installed.installed ? "Add from SD" : "Install from SD";
    case DualBootActionOnline:
        return "Online search";
    case DualBootActionDelete:
        return "Delete firmware";
    case DualBootActionRecovery:
        return "Recovery 2.0 info";
    default:
        return "";
    }
}

static void dualboot_render_secondary(Canvas* canvas, DualBoot* app) {
    char title[26];
    snprintf(
        title,
        sizeof(title),
        "APP: %.18s",
        app->installed.installed ? app->installed.project : "Empty");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 7, AlignCenter, AlignCenter, title);
    canvas_set_font(canvas, FontSecondary);
    const uint8_t count = dualboot_action_count(app);
    for(uint8_t i = 0; i < count; ++i) {
        const uint8_t y = count > 4 ? 18 + i * 10 : 22 + i * 12;
        canvas_draw_str_aligned(
            canvas,
            64,
            y,
            AlignCenter,
            AlignCenter,
            dualboot_action_label(app, dualboot_action_at(app, i)));
        if(app->menu_idx == i) elements_frame(canvas, 4, y - 5, 120, count > 4 ? 10 : 12);
    }
}

static void dualboot_render_files(Canvas* canvas, DualBoot* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignCenter, "Choose firmware");
    canvas_set_font(canvas, FontSecondary);
    if(app->count == 0) {
        canvas_draw_str_aligned(canvas, 64, 27, AlignCenter, AlignCenter, "No .bin files found");
        canvas_draw_str_aligned(canvas, 64, 41, AlignCenter, AlignCenter, "Copy firmware to");
        canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignCenter, "SD:/firmware");
        return;
    }
    for(size_t row = 0; row < DUALBOOT_VISIBLE; ++row) {
        const size_t index = app->top + row;
        if(index >= app->count) break;
        char name[25];
        dualboot_short_text(name, sizeof(name), app->names[index], 20);
        const uint8_t y = 19 + (uint8_t)(row * 12);
        canvas_draw_str_aligned(canvas, 64, y, AlignCenter, AlignCenter, name);
        if(index == app->idx) elements_frame(canvas, 2, y - 6, 124, 12);
    }
}

static void dualboot_render_online(Canvas* canvas, DualBoot* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignCenter, "Online: T-Embed only");
    canvas_set_font(canvas, FontSecondary);
    for(size_t row = 0; row < DUALBOOT_VISIBLE; ++row) {
        const size_t index = app->online_top + row;
        if(index >= app->online_count) break;
        char label[30];
        snprintf(
            label,
            sizeof(label),
            "%.18s / %.7s",
            app->online[index].name,
            app->online[index].author);
        const uint8_t y = 19 + (uint8_t)(row * 12);
        canvas_draw_str_aligned(canvas, 64, y, AlignCenter, AlignCenter, label);
        if(index == app->online_idx) elements_frame(canvas, 2, y - 6, 124, 12);
    }
}

static void dualboot_render_confirm_online(Canvas* canvas, DualBoot* app) {
    const DualBootOnlineItem* item = &app->online[app->online_idx];
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 7, AlignCenter, AlignCenter, "Download latest?");
    char name[26];
    char author[26];
    dualboot_short_text(name, sizeof(name), item->name, 21);
    dualboot_short_text(author, sizeof(author), item->author, 21);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 23, AlignCenter, AlignCenter, name);
    canvas_draw_str_aligned(canvas, 64, 35, AlignCenter, AlignCenter, author);
    canvas_draw_str_aligned(canvas, 64, 46, AlignCenter, AlignCenter, "Saved + checked on SD");
    static const char* const labels[] = {"Cancel", "Download"};
    for(uint8_t i = 0; i < 2; ++i) {
        const uint8_t x = i ? 92 : 36;
        canvas_draw_str_aligned(canvas, x, 59, AlignCenter, AlignCenter, labels[i]);
        if(app->confirm_idx == i) elements_frame(canvas, x - 25, 52, 50, 12);
    }
}

static void dualboot_render_confirm_install(Canvas* canvas, DualBoot* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas,
        64,
        7,
        AlignCenter,
        AlignCenter,
        app->installed.installed ? "Add firmware?" : "Install firmware?");
    char name[24];
    char project[24];
    dualboot_short_text(name, sizeof(name), app->names[app->idx], 19);
    dualboot_short_text(project, sizeof(project), app->pending.project, 19);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 21, AlignCenter, AlignCenter, name);
    canvas_draw_str_aligned(canvas, 64, 33, AlignCenter, AlignCenter, project);
    char size[24];
    snprintf(size, sizeof(size), "%lu KiB; free %lu", (unsigned long)(app->pending.length / 1024), (unsigned long)(multiboot_largest_gap() / 1024));
    canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, size);
    static const char* const labels[] = {"Cancel", "Install"};
    for(uint8_t i = 0; i < 2; ++i) {
        const uint8_t x = i ? 92 : 36;
        canvas_draw_str_aligned(canvas, x, 58, AlignCenter, AlignCenter, labels[i]);
        if(app->confirm_idx == i) elements_frame(canvas, x - 25, 51, 50, 12);
    }
}

static void dualboot_render_confirm_delete(Canvas* canvas, DualBoot* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignCenter, "Remove this firmware?");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, app->installed.project);
    canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Factory stays protected");
    static const char* const labels[] = {"Cancel", "Delete"};
    for(uint8_t i = 0; i < 2; ++i) {
        const uint8_t x = i ? 92 : 36;
        canvas_draw_str_aligned(canvas, x, 57, AlignCenter, AlignCenter, labels[i]);
        if(app->confirm_idx == i) elements_frame(canvas, x - 25, 50, 50, 12);
    }
}

static void dualboot_render_working(Canvas* canvas, DualBoot* app) {
    const char* title = "Working safely";
    const char* footer = "Flipper stays protected";
    if(app->work == DualBootWorkInstall) {
        title = "Installing safely";
        footer = "Do not power off";
    } else if(app->work == DualBootWorkDelete) {
        title = "Removing firmware";
        footer = "Do not power off";
    } else if(app->work == DualBootWorkCatalog) {
        title = "Online search";
        footer = "WiFi on / BLE off";
    } else if(app->work == DualBootWorkDownload) {
        title = "Downloading to SD";
        footer = "Slot is not touched yet";
    }
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas,
        64,
        12,
        AlignCenter,
        AlignCenter,
        title);
    elements_progress_bar(canvas, 8, 31, 112, app->progress);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 49, AlignCenter, AlignCenter, app->message);
    canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignCenter, footer);
}

static void dualboot_render_done(Canvas* canvas, DualBoot* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 12, AlignCenter, AlignCenter, app->message);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, "SD firmware file retained");
    canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, "Restart to load new layout");
    canvas_draw_str_aligned(canvas, 64, 59, AlignCenter, AlignCenter, "OK / Back: restart");
}

static void dualboot_render_error(Canvas* canvas, DualBoot* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 12, AlignCenter, AlignCenter, "Recovery blocked boot");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 31, AlignCenter, AlignCenter, app->message);
    canvas_draw_str_aligned(canvas, 64, 45, AlignCenter, AlignCenter, "Flipper remains selected");
    canvas_draw_str_aligned(canvas, 64, 59, AlignCenter, AlignCenter, "Press Back");
}

static void dualboot_render_help(Canvas* canvas) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 7, AlignCenter, AlignCenter, "MULTI-BOOT RECOVERY");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 21, "Bad/corrupt files: blocked");
    canvas_draw_str(canvas, 2, 32, "Factory stays protected");
    canvas_draw_str(canvas, 2, 43, "Press RESET, then hold BACK");
    canvas_draw_str(canvas, 2, 54, "until Flipper boots.");
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK / Back");
}

static void dualboot_render_callback(Canvas* canvas, void* context) {
    DualBoot* app = context;
    if(furi_mutex_acquire(app->mutex, 200) != FuriStatusOk) return;
    switch(app->state) {
    case DualBootStateWarning:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignCenter, "DUAL BOOT WARNING");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 22, "To return to Flipper:");
        canvas_draw_str(canvas, 2, 32, "Press RESET, then press");
        canvas_draw_str(canvas, 2, 42, "and hold BACK until");
        canvas_draw_str(canvas, 2, 52, "Flipper boots.");
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: Continue  Back: Exit");
        break;
    case DualBootStateHome:
        dualboot_render_home(canvas, app);
        break;
    case DualBootStateBootChoices:
        dualboot_render_boot_choices(canvas, app);
        break;
    case DualBootStateSecondary:
        dualboot_render_secondary(canvas, app);
        break;
    case DualBootStateFiles:
        dualboot_render_files(canvas, app);
        break;
    case DualBootStateOnline:
        dualboot_render_online(canvas, app);
        break;
    case DualBootStateConfirmOnline:
        dualboot_render_confirm_online(canvas, app);
        break;
    case DualBootStateConfirmInstall:
        dualboot_render_confirm_install(canvas, app);
        break;
    case DualBootStateConfirmDelete:
        dualboot_render_confirm_delete(canvas, app);
        break;
    case DualBootStateWorking:
        dualboot_render_working(canvas, app);
        break;
    case DualBootStateDone:
        dualboot_render_done(canvas, app);
        break;
    case DualBootStateError:
        dualboot_render_error(canvas, app);
        break;
    case DualBootStateHelp:
        dualboot_render_help(canvas);
        break;
    }
    furi_mutex_release(app->mutex);
}

static void dualboot_input_callback(InputEvent* input, void* context) {
    DualBoot* app = context;
    const DualBootEvent event = {.type = DualBootEventKey, .input = *input};
    furi_message_queue_put(app->queue, &event, FuriWaitForever);
}

/* -------------------------------------------------------------------------- */
/* Input/state transitions                                                      */
/* -------------------------------------------------------------------------- */

static void dualboot_scroll_to(size_t* top, size_t index) {
    if(index < *top) {
        *top = index;
    } else if(index >= *top + DUALBOOT_VISIBLE) {
        *top = index - DUALBOOT_VISIBLE + 1;
    }
}

static const char* dualboot_boot_error(esp_err_t err) {
    if(err == ESP_ERR_OTA_VALIDATE_FAILED) return "Secondary image is corrupt";
    if(err == ESP_ERR_NO_MEM) return "Low RAM: reboot & retry";
    return esp_err_to_name(err);
}

static void dualboot_boot_secondary(DualBoot* app, DualBootState return_state) {
    const esp_err_t err = dualboot_select_secondary(app);
    if(err == ESP_OK) {
        FURI_LOG_I(TAG, "Booting selected installed firmware");
        fw_ota_reboot_async(100);
        return;
    }
    dualboot_set_error(app, dualboot_boot_error(err), return_state);
}

static void dualboot_choose_boot(DualBoot* app) {
    if(dualboot_choice_is_installed(app, app->boot_idx)) {
        app->selected_slot = app->boot_idx;
        app->installed = app->slots[app->selected_slot];
        dualboot_boot_secondary(app, DualBootStateBootChoices);
        return;
    }

    const size_t file_index = dualboot_choice_file_index(app, app->boot_idx);
    if(!dualboot_prepare_file(app, file_index, DualBootStateBootChoices)) return;
    app->confirm_idx = 0;
    app->install_return = DualBootStateBootChoices;
    app->state = DualBootStateConfirmInstall;
}

static bool dualboot_quick_route(DualBoot* app);

static bool dualboot_handle_key(DualBoot* app, const InputEvent* input) {
    if(input->type != InputTypeShort && input->type != InputTypeRepeat &&
       input->type != InputTypeLong) {
        return true;
    }
    const bool nav = input->type == InputTypeShort || input->type == InputTypeRepeat;
    const bool accept = input->type == InputTypeShort && input->key == InputKeyOk;

    switch(app->state) {
    case DualBootStateWarning:
        if(input->type == InputTypeShort && input->key == InputKeyBack) return false;
        if(accept) {
            app->state = DualBootStateHome;
            if(app->quick_mode) dualboot_quick_route(app);
        }
        break;
    case DualBootStateHome:
        if(input->type == InputTypeShort && input->key == InputKeyBack) return false;
        if(nav && (input->key == InputKeyUp || input->key == InputKeyDown)) {
            const uint8_t count = app->installed_count + 3;
            app->home_tab = input->key == InputKeyUp ?
                (app->home_tab ? app->home_tab - 1 : count - 1) : (app->home_tab + 1) % count;
            if(app->home_tab < app->home_top) app->home_top = app->home_tab;
            else if(app->home_tab >= app->home_top + DUALBOOT_VISIBLE)
                app->home_top = app->home_tab - DUALBOOT_VISIBLE + 1;
        } else if(accept) {
            if(app->home_tab == 0) {
                app->return_state = DualBootStateHome;
                app->state = DualBootStateHelp;
            } else if(app->home_tab <= app->installed_count) {
                app->selected_slot = app->home_tab - 1;
                app->installed = app->slots[app->selected_slot];
                app->menu_idx = 0;
                app->state = DualBootStateSecondary;
            } else if(app->home_tab == app->installed_count + 1U) {
                dualboot_scan_or_create(app);
                app->state = DualBootStateFiles;
            } else {
                dualboot_start_worker(app, DualBootWorkCatalog, false);
            }
        }
        break;

    case DualBootStateBootChoices: {
        const size_t count = dualboot_boot_choice_count(app);
        if(input->type == InputTypeShort && input->key == InputKeyBack) return false;
        if(count == 0) {
            if(accept) {
                app->quick_mode = false;
                app->state = DualBootStateHome;
            }
            break;
        }
        if(nav && input->key == InputKeyUp) {
            app->boot_idx = app->boot_idx == 0 ? count - 1 : app->boot_idx - 1;
            dualboot_scroll_to(&app->boot_top, app->boot_idx);
        } else if(nav && input->key == InputKeyDown) {
            app->boot_idx = app->boot_idx + 1 >= count ? 0 : app->boot_idx + 1;
            dualboot_scroll_to(&app->boot_top, app->boot_idx);
        } else if(accept) {
            dualboot_choose_boot(app);
        }
        break;
    }

    case DualBootStateSecondary:
        if(input->type == InputTypeShort && input->key == InputKeyBack) {
            app->state = DualBootStateHome;
            break;
        }
        if(nav && (input->key == InputKeyUp || input->key == InputKeyDown)) {
            const uint8_t count = dualboot_action_count(app);
            if(input->key == InputKeyUp) {
                app->menu_idx = app->menu_idx == 0 ? count - 1 : app->menu_idx - 1;
            } else {
                app->menu_idx = app->menu_idx + 1 >= count ? 0 : app->menu_idx + 1;
            }
        } else if(accept) {
            switch(dualboot_action_at(app, app->menu_idx)) {
            case DualBootActionBoot:
                dualboot_boot_secondary(app, DualBootStateSecondary);
                break;
            case DualBootActionInstall:
                dualboot_scan_or_create(app);
                app->state = DualBootStateFiles;
                break;
            case DualBootActionOnline:
                dualboot_start_worker(app, DualBootWorkCatalog, false);
                break;
            case DualBootActionDelete:
                app->confirm_idx = 0;
                app->state = DualBootStateConfirmDelete;
                break;
            case DualBootActionRecovery:
                app->return_state = DualBootStateSecondary;
                app->state = DualBootStateHelp;
                break;
            }
        }
        break;

    case DualBootStateFiles:
        if(input->type == InputTypeShort && input->key == InputKeyBack) {
            app->state = DualBootStateSecondary;
            break;
        }
        if(app->count == 0) break;
        if(nav && input->key == InputKeyUp) {
            app->idx = app->idx == 0 ? app->count - 1 : app->idx - 1;
            dualboot_scroll_to(&app->top, app->idx);
        } else if(nav && input->key == InputKeyDown) {
            app->idx = app->idx + 1 >= app->count ? 0 : app->idx + 1;
            dualboot_scroll_to(&app->top, app->idx);
        } else if(accept && dualboot_prepare_file(app, app->idx, DualBootStateFiles)) {
            app->confirm_idx = 0;
            app->boot_after_install = false;
            app->install_return = DualBootStateFiles;
            app->state = DualBootStateConfirmInstall;
        }
        break;

    case DualBootStateOnline:
        if(input->type == InputTypeShort && input->key == InputKeyBack) {
            app->state = DualBootStateSecondary;
            break;
        }
        if(app->online_count == 0) break;
        if(nav && input->key == InputKeyUp) {
            app->online_idx =
                app->online_idx == 0 ? app->online_count - 1 : app->online_idx - 1;
            dualboot_scroll_to(&app->online_top, app->online_idx);
        } else if(nav && input->key == InputKeyDown) {
            app->online_idx =
                app->online_idx + 1 >= app->online_count ? 0 : app->online_idx + 1;
            dualboot_scroll_to(&app->online_top, app->online_idx);
        } else if(accept) {
            app->confirm_idx = 0;
            app->state = DualBootStateConfirmOnline;
        }
        break;

    case DualBootStateConfirmOnline:
        if(input->type == InputTypeShort && input->key == InputKeyBack) {
            app->state = DualBootStateOnline;
        } else if(nav && (input->key == InputKeyUp || input->key == InputKeyDown)) {
            app->confirm_idx ^= 1U;
        } else if(accept) {
            if(app->confirm_idx == 0) {
                app->state = DualBootStateOnline;
            } else {
                dualboot_start_worker(app, DualBootWorkDownload, false);
            }
        }
        break;

    case DualBootStateConfirmInstall:
        if(input->type == InputTypeShort && input->key == InputKeyBack) {
            app->state = app->install_return;
        } else if(nav && (input->key == InputKeyUp || input->key == InputKeyDown)) {
            app->confirm_idx ^= 1U;
        } else if(accept) {
            if(app->confirm_idx == 0) {
                app->state = app->install_return;
            } else {
                const bool boot_after = app->boot_after_install;
                dualboot_start_worker(app, DualBootWorkInstall, boot_after);
            }
        }
        break;

    case DualBootStateConfirmDelete:
        if(input->type == InputTypeShort && input->key == InputKeyBack) {
            app->state = DualBootStateSecondary;
        } else if(nav && (input->key == InputKeyUp || input->key == InputKeyDown)) {
            app->confirm_idx ^= 1U;
        } else if(accept) {
            if(app->confirm_idx == 0) {
                app->state = DualBootStateSecondary;
            } else {
                dualboot_start_worker(app, DualBootWorkDelete, false);
            }
        }
        break;

    case DualBootStateWorking:
        break;

    case DualBootStateDone:
        if(accept || (input->type == InputTypeShort && input->key == InputKeyBack)) {
            fw_ota_reboot_async(100);
        }
        break;

    case DualBootStateError:
        if(accept || (input->type == InputTypeShort && input->key == InputKeyBack)) {
            app->state = app->error_return;
        }
        break;

    case DualBootStateHelp:
        if(accept || (input->type == InputTypeShort && input->key == InputKeyBack)) {
            app->state = app->return_state;
        }
        break;
    }
    return true;
}

static bool dualboot_quick_route(DualBoot* app) {
    const size_t count = dualboot_boot_choice_count(app);
    if(count == 0) {
        app->state = DualBootStateBootChoices;
        return true;
    }
    if(count > 1) {
        app->state = DualBootStateBootChoices;
        return true;
    }

    /* Exactly one named choice: match the requested shortcut behavior. */
    app->boot_idx = 0;
    if(dualboot_choice_is_installed(app, 0)) {
        const esp_err_t err = dualboot_select_secondary(app);
        if(err == ESP_OK) {
            FURI_LOG_I(TAG, "One firmware found; direct boot after warning");
            fw_ota_reboot_async(100);
            return true;
        }
        dualboot_set_error(app, dualboot_boot_error(err), DualBootStateBootChoices);
        return true;
    }

    if(!dualboot_prepare_file(app, 0, DualBootStateBootChoices)) return true;
    dualboot_start_worker(app, DualBootWorkInstall, true);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                 */
/* -------------------------------------------------------------------------- */

int32_t dualboot_app(void* argument) {
    DualBoot* app = heap_caps_calloc(
        1, sizeof(DualBoot), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!app) return -1;
    app->names = heap_caps_calloc(
        DUALBOOT_MAX_FIRMWARES,
        DUALBOOT_NAME_LEN,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    app->online = heap_caps_calloc(
        DUALBOOT_ONLINE_MAX,
        sizeof(DualBootOnlineItem),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    app->online_url = heap_caps_calloc(
        DUALBOOT_ONLINE_URL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!app->names || !app->online || !app->online_url) {
        heap_caps_free(app->online_url);
        heap_caps_free(app->online);
        heap_caps_free(app->names);
        heap_caps_free(app);
        return -1;
    }

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(DualBootEvent));
    app->state = DualBootStateWarning;
    app->return_state = DualBootStateHome;
    app->error_return = DualBootStateHome;
    app->install_return = DualBootStateFiles;
    app->home_tab = 0;
    app->running = true;
    app->quick_mode = argument && strcmp((const char*)argument, "quick") == 0;
    dualboot_refresh_installed(app);
    dualboot_scan_or_create(app);
    if(!multiboot_supported()) {
        app->state = DualBootStateError;
        strlcpy(app->message, "Multi-boot layout required", sizeof(app->message));
    }

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, dualboot_render_callback, app);
    view_port_input_callback_set(app->view_port, dualboot_input_callback, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    FURI_LOG_I(
        TAG,
        "Opened: choices=%u internal_free=%u largest=%u SD+online_catalogue=PSRAM",
        (unsigned)dualboot_boot_choice_count(app),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DualBootEvent event;
    while(app->running) {
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) != FuriStatusOk) continue;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(event.type == DualBootEventKey) {
            app->running = dualboot_handle_key(app, &event.input);
        } else if(event.type == DualBootEventFinished && app->worker) {
            furi_thread_join(app->worker);
            furi_thread_free(app->worker);
            app->worker = NULL;
        }
        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
    }

    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    heap_caps_free(app->online_url);
    heap_caps_free(app->online);
    heap_caps_free(app->names);
    heap_caps_free(app);
    return 0;
}
