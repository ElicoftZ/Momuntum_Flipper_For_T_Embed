#include "hotspot_arcade_config.h"

#include <flipper_format/flipper_format.h>
#include <furi.h>
#include <string.h>

#define HOTSPOT_ARCADE_CONFIG_TYPE "Hotspot Arcade Config"
#define HOTSPOT_ARCADE_CONFIG_VERSION 1U

static void hotspot_arcade_copy_string(char* destination, size_t capacity, const char* source) {
    if(!destination || capacity == 0U) return;

    size_t index = 0U;
    if(source) {
        while((index + 1U < capacity) && source[index]) {
            destination[index] = source[index];
            index++;
        }
    }
    destination[index] = '\0';
}

static bool hotspot_arcade_lang_is_valid(const char* lang) {
    return !strcmp(lang, "") || !strcmp(lang, "de") || !strcmp(lang, "pt-br");
}

void hotspot_arcade_settings_set_defaults(HotspotArcadeSettings* settings) {
    furi_check(settings);
    memset(settings, 0, sizeof(*settings));
    hotspot_arcade_copy_string(settings->ssid, sizeof(settings->ssid), "Hotspot Arcade");
    settings->sound = true;
    settings->vibro = true;
}

bool hotspot_arcade_settings_load(Storage* storage, HotspotArcadeSettings* settings) {
    furi_check(storage);
    furi_check(settings);

    hotspot_arcade_settings_set_defaults(settings);
    if(!storage_file_exists(storage, HOTSPOT_ARCADE_CONFIG_PATH)) return false;

    bool loaded = false;
    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* file_type = furi_string_alloc();
    FuriString* value = furi_string_alloc();
    uint32_t version = 0U;

    do {
        if(!flipper_format_file_open_existing(file, HOTSPOT_ARCADE_CONFIG_PATH)) break;
        if(!flipper_format_read_header(file, file_type, &version)) break;
        if(strcmp(furi_string_get_cstr(file_type), HOTSPOT_ARCADE_CONFIG_TYPE) ||
           version != HOTSPOT_ARCADE_CONFIG_VERSION) {
            break;
        }

        if(flipper_format_read_string(file, "SSID", value) && furi_string_size(value) > 0U) {
            hotspot_arcade_copy_string(
                settings->ssid, sizeof(settings->ssid), furi_string_get_cstr(value));
        }

        furi_string_reset(value);
        if(flipper_format_read_string(file, "Language", value)) {
            const char* lang = furi_string_get_cstr(value);
            if(!strcmp(lang, "en")) lang = "";
            if(hotspot_arcade_lang_is_valid(lang)) {
                hotspot_arcade_copy_string(settings->lang, sizeof(settings->lang), lang);
            }
        }

        bool flag = false;
        if(flipper_format_read_bool(file, "Sound", &flag, 1U)) settings->sound = flag;
        if(flipper_format_read_bool(file, "Vibro", &flag, 1U)) settings->vibro = flag;
        loaded = true;
    } while(false);

    flipper_format_free(file);
    furi_string_free(value);
    furi_string_free(file_type);
    return loaded;
}

bool hotspot_arcade_settings_save(Storage* storage, const HotspotArcadeSettings* settings) {
    furi_check(storage);
    furi_check(settings);

    if(!storage_dir_exists(storage, EXT_PATH("apps_data")) &&
       !storage_simply_mkdir(storage, EXT_PATH("apps_data"))) {
        return false;
    }
    if(!storage_dir_exists(storage, HOTSPOT_ARCADE_CONFIG_DIR) &&
       !storage_simply_mkdir(storage, HOTSPOT_ARCADE_CONFIG_DIR)) {
        return false;
    }

    if(storage_file_exists(storage, HOTSPOT_ARCADE_CONFIG_PATH)) {
        storage_common_remove(storage, HOTSPOT_ARCADE_CONFIG_PATH);
    }

    bool saved = false;
    FlipperFormat* file = flipper_format_file_alloc(storage);
    const char* stored_lang = settings->lang[0] ? settings->lang : "en";

    do {
        if(!flipper_format_file_open_new(file, HOTSPOT_ARCADE_CONFIG_PATH)) break;
        if(!flipper_format_write_header_cstr(
               file, HOTSPOT_ARCADE_CONFIG_TYPE, HOTSPOT_ARCADE_CONFIG_VERSION)) {
            break;
        }
        if(!flipper_format_write_string_cstr(file, "SSID", settings->ssid)) break;
        if(!flipper_format_write_string_cstr(file, "Language", stored_lang)) break;
        if(!flipper_format_write_bool(file, "Sound", &settings->sound, 1U)) break;
        if(!flipper_format_write_bool(file, "Vibro", &settings->vibro, 1U)) break;
        saved = true;
    } while(false);

    flipper_format_free(file);
    return saved;
}
