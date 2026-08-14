#include "settings_i.h"

#include <flipper_format/flipper_format.h>
#include <furi.h>

#include <string.h>

#define TAG "MomentumSettings"

/* Zero-initialised here and given its table defaults by
 * momentum_settings_set_defaults() at the top of app_main, before any service
 * that reads it can start. momentum_settings_load() then overlays the file. */
MomentumSettings momentum_settings;
uint32_t momentum_settings_revision = 0;

static char* momentum_settings_field(MomentumSettings* settings, const MomentumSettingsEntry* entry) {
    return (char*)settings + entry->offset;
}

static void momentum_settings_copy_str(
    MomentumSettings* settings,
    const MomentumSettingsEntry* entry,
    FuriString* source) {
    char* field = momentum_settings_field(settings, entry);
    size_t length = furi_string_size(source);
    if(length >= entry->size) length = entry->size - 1;

    memcpy(field, furi_string_get_cstr(source), length);
    field[length] = '\0';
}

void momentum_settings_load(void) {
    const MomentumSettings previous_settings = momentum_settings;
    momentum_settings_set_defaults(&momentum_settings);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* string_value = furi_string_alloc();

    const bool opened = flipper_format_file_open_existing(file, MOMENTUM_SETTINGS_PATH);
    if(opened) {
        for(size_t i = 0; i < momentum_settings_entry_count; ++i) {
            const MomentumSettingsEntry* entry = &momentum_settings_entries[i];
            int32_t int_value;
            uint32_t uint_value;
            bool bool_value;

            /* Rewind per entry so a key missing from an older or hand-edited
             * file cannot consume the position of the keys after it. */
            if(!flipper_format_rewind(file)) break;

            switch(entry->type) {
            case MomentumSettingsTypeStr:
                if(flipper_format_read_string(file, entry->key, string_value)) {
                    momentum_settings_copy_str(&momentum_settings, entry, string_value);
                }
                break;
            case MomentumSettingsTypeInt:
                if(flipper_format_read_int32(file, entry->key, &int_value, 1)) {
                    momentum_settings_entry_set(entry, &momentum_settings, int_value);
                }
                break;
            case MomentumSettingsTypeUint:
            case MomentumSettingsTypeEnum:
                if(flipper_format_read_uint32(file, entry->key, &uint_value, 1)) {
                    momentum_settings_entry_set(entry, &momentum_settings, (int32_t)uint_value);
                }
                break;
            case MomentumSettingsTypeBool:
                if(flipper_format_read_bool(file, entry->key, &bool_value, 1)) {
                    momentum_settings_entry_set(entry, &momentum_settings, bool_value ? 1 : 0);
                }
                break;
            }
        }
    }

    momentum_settings_sanitize(&momentum_settings);
    if(!momentum_settings_equal(&previous_settings, &momentum_settings)) {
        momentum_settings_revision++;
    }

    FURI_LOG_I(
        TAG,
        "%s: pack='%s', speed=%lu, cycle=%ld, unlock=%s, menu=%u",
        opened ? "Loaded" : "Defaults",
        momentum_settings.asset_pack,
        (unsigned long)momentum_settings.anim_speed,
        (long)momentum_settings.cycle_anims,
        momentum_settings.unlock_anims ? "on" : "off",
        (unsigned)momentum_settings.menu_style);

    furi_string_free(string_value);
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
}

bool momentum_settings_save(void) {
    momentum_settings_sanitize(&momentum_settings);
    momentum_settings_revision++;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool success = flipper_format_file_open_always(file, MOMENTUM_SETTINGS_PATH);

    for(size_t i = 0; success && i < momentum_settings_entry_count; ++i) {
        const MomentumSettingsEntry* entry = &momentum_settings_entries[i];
        const int64_t value = momentum_settings_entry_get(entry, &momentum_settings);
        int32_t int_value = (int32_t)value;
        uint32_t uint_value = (uint32_t)value;
        bool bool_value = value != 0;

        switch(entry->type) {
        case MomentumSettingsTypeStr:
            success = flipper_format_write_string_cstr(
                file, entry->key, momentum_settings_field(&momentum_settings, entry));
            break;
        case MomentumSettingsTypeInt:
            success = flipper_format_write_int32(file, entry->key, &int_value, 1);
            break;
        case MomentumSettingsTypeUint:
        case MomentumSettingsTypeEnum:
            success = flipper_format_write_uint32(file, entry->key, &uint_value, 1);
            break;
        case MomentumSettingsTypeBool:
            success = flipper_format_write_bool(file, entry->key, &bool_value, 1);
            break;
        }
    }

    if(!success) {
        FURI_LOG_E(TAG, "Failed to save %s", MOMENTUM_SETTINGS_PATH);
    }

    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    return success;
}
