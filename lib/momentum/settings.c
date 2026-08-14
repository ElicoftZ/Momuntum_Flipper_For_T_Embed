#include "settings_i.h"

#include <flipper_format/flipper_format.h>
#include <furi.h>

#include <string.h>

#define TAG "MomentumSettings"

MomentumSettings momentum_settings = {
    .asset_pack = "",
    .anim_speed = MOMENTUM_ANIM_SPEED_DEFAULT,
    .cycle_anims = MOMENTUM_CYCLE_ANIMS_DEFAULT,
    .unlock_anims = false,
    .menu_style = MenuStyleList,
    .lockscreen_poweroff = true,
    .lockscreen_time = true,
    .lockscreen_seconds = false,
    .lockscreen_date = true,
    .lockscreen_statusbar = true,
    .lockscreen_prompt = true,
    .lockscreen_transparent = false,
    .lockscreen_skip_animation = false,
    .scroll_marquee = false,
    .midnight_format_00 = true,
    .popup_overlay = true,
    .status_icons = true,
    .bar_borders = true,
    .bar_background = false,
    .sort_dirs_first = true,
    .show_hidden_files = false,
    .show_internal_tab = false,
    .browser_path_mode = BrowserPathOff,
    .dark_mode = false,
    .file_naming_prefix_after = false,
};
uint32_t momentum_settings_revision = 0;

static bool momentum_settings_equal(
    const MomentumSettings* left,
    const MomentumSettings* right) {
    return !strcmp(left->asset_pack, right->asset_pack) &&
           left->anim_speed == right->anim_speed &&
           left->cycle_anims == right->cycle_anims &&
           left->unlock_anims == right->unlock_anims &&
           left->menu_style == right->menu_style &&
           left->lockscreen_poweroff == right->lockscreen_poweroff &&
           left->lockscreen_time == right->lockscreen_time &&
           left->lockscreen_seconds == right->lockscreen_seconds &&
           left->lockscreen_date == right->lockscreen_date &&
           left->lockscreen_statusbar == right->lockscreen_statusbar &&
           left->lockscreen_prompt == right->lockscreen_prompt &&
           left->lockscreen_transparent == right->lockscreen_transparent &&
           left->lockscreen_skip_animation == right->lockscreen_skip_animation &&
           left->scroll_marquee == right->scroll_marquee &&
           left->midnight_format_00 == right->midnight_format_00 &&
           left->popup_overlay == right->popup_overlay &&
           left->status_icons == right->status_icons &&
           left->bar_borders == right->bar_borders &&
           left->bar_background == right->bar_background &&
           left->sort_dirs_first == right->sort_dirs_first &&
           left->show_hidden_files == right->show_hidden_files &&
           left->show_internal_tab == right->show_internal_tab &&
           left->browser_path_mode == right->browser_path_mode &&
           left->dark_mode == right->dark_mode &&
           left->file_naming_prefix_after == right->file_naming_prefix_after;
}

static void momentum_settings_copy_pack_name(FuriString* source) {
    const char* value = furi_string_get_cstr(source);
    size_t length = furi_string_size(source);
    if(length >= sizeof(momentum_settings.asset_pack)) {
        length = sizeof(momentum_settings.asset_pack) - 1;
    }

    memcpy(momentum_settings.asset_pack, value, length);
    momentum_settings.asset_pack[length] = '\0';
}

void momentum_settings_load(void) {
    const MomentumSettings previous_settings = momentum_settings;
    momentum_settings_set_defaults(&momentum_settings);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* string_value = furi_string_alloc();

    bool opened = flipper_format_file_open_existing(file, MOMENTUM_SETTINGS_PATH);
    if(opened) {
        uint32_t uint_value;
        int32_t int_value;
        bool bool_value;

        if(flipper_format_rewind(file) &&
           flipper_format_read_string(file, "asset_pack", string_value)) {
            momentum_settings_copy_pack_name(string_value);
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_uint32(file, "anim_speed", &uint_value, 1)) {
            momentum_settings.anim_speed = uint_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_int32(file, "cycle_anims", &int_value, 1)) {
            momentum_settings.cycle_anims = int_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "unlock_anims", &bool_value, 1)) {
            momentum_settings.unlock_anims = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_uint32(file, "menu_style", &uint_value, 1)) {
            momentum_settings.menu_style = (MenuStyle)uint_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "lockscreen_poweroff", &bool_value, 1)) {
            momentum_settings.lockscreen_poweroff = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "lockscreen_time", &bool_value, 1)) {
            momentum_settings.lockscreen_time = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "lockscreen_seconds", &bool_value, 1)) {
            momentum_settings.lockscreen_seconds = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "lockscreen_date", &bool_value, 1)) {
            momentum_settings.lockscreen_date = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "lockscreen_statusbar", &bool_value, 1)) {
            momentum_settings.lockscreen_statusbar = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "lockscreen_prompt", &bool_value, 1)) {
            momentum_settings.lockscreen_prompt = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "lockscreen_transparent", &bool_value, 1)) {
            momentum_settings.lockscreen_transparent = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "lockscreen_skip_animation", &bool_value, 1)) {
            momentum_settings.lockscreen_skip_animation = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "scroll_marquee", &bool_value, 1)) {
            momentum_settings.scroll_marquee = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "midnight_format_00", &bool_value, 1)) {
            momentum_settings.midnight_format_00 = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "popup_overlay", &bool_value, 1)) {
            momentum_settings.popup_overlay = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "status_icons", &bool_value, 1)) {
            momentum_settings.status_icons = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "bar_borders", &bool_value, 1)) {
            momentum_settings.bar_borders = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "bar_background", &bool_value, 1)) {
            momentum_settings.bar_background = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "sort_dirs_first", &bool_value, 1)) {
            momentum_settings.sort_dirs_first = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "show_hidden_files", &bool_value, 1)) {
            momentum_settings.show_hidden_files = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "show_internal_tab", &bool_value, 1)) {
            momentum_settings.show_internal_tab = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_uint32(file, "browser_path_mode", &uint_value, 1)) {
            momentum_settings.browser_path_mode = (BrowserPathMode)uint_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "dark_mode", &bool_value, 1)) {
            momentum_settings.dark_mode = bool_value;
        }

        if(flipper_format_rewind(file) &&
           flipper_format_read_bool(file, "file_naming_prefix_after", &bool_value, 1)) {
            momentum_settings.file_naming_prefix_after = bool_value;
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

    if(success) {
        const uint32_t menu_style = (uint32_t)momentum_settings.menu_style;
        success &= flipper_format_write_string_cstr(
            file, "asset_pack", momentum_settings.asset_pack);
        success &= flipper_format_write_uint32(
            file, "anim_speed", &momentum_settings.anim_speed, 1);
        success &= flipper_format_write_int32(
            file, "cycle_anims", &momentum_settings.cycle_anims, 1);
        success &= flipper_format_write_bool(
            file, "unlock_anims", &momentum_settings.unlock_anims, 1);
        success &= flipper_format_write_uint32(file, "menu_style", &menu_style, 1);
        success &= flipper_format_write_bool(
            file, "lockscreen_poweroff", &momentum_settings.lockscreen_poweroff, 1);
        success &= flipper_format_write_bool(
            file, "lockscreen_time", &momentum_settings.lockscreen_time, 1);
        success &= flipper_format_write_bool(
            file, "lockscreen_seconds", &momentum_settings.lockscreen_seconds, 1);
        success &= flipper_format_write_bool(
            file, "lockscreen_date", &momentum_settings.lockscreen_date, 1);
        success &= flipper_format_write_bool(
            file, "lockscreen_statusbar", &momentum_settings.lockscreen_statusbar, 1);
        success &= flipper_format_write_bool(
            file, "lockscreen_prompt", &momentum_settings.lockscreen_prompt, 1);
        success &= flipper_format_write_bool(
            file, "lockscreen_transparent", &momentum_settings.lockscreen_transparent, 1);
        success &= flipper_format_write_bool(
            file,
            "lockscreen_skip_animation",
            &momentum_settings.lockscreen_skip_animation,
            1);
        success &= flipper_format_write_bool(
            file, "scroll_marquee", &momentum_settings.scroll_marquee, 1);
        success &= flipper_format_write_bool(
            file, "midnight_format_00", &momentum_settings.midnight_format_00, 1);
        success &= flipper_format_write_bool(
            file, "popup_overlay", &momentum_settings.popup_overlay, 1);
        success &= flipper_format_write_bool(
            file, "status_icons", &momentum_settings.status_icons, 1);
        success &= flipper_format_write_bool(
            file, "bar_borders", &momentum_settings.bar_borders, 1);
        success &= flipper_format_write_bool(
            file, "bar_background", &momentum_settings.bar_background, 1);
        success &= flipper_format_write_bool(
            file, "sort_dirs_first", &momentum_settings.sort_dirs_first, 1);
        success &= flipper_format_write_bool(
            file, "show_hidden_files", &momentum_settings.show_hidden_files, 1);
        success &= flipper_format_write_bool(
            file, "show_internal_tab", &momentum_settings.show_internal_tab, 1);
        const uint32_t browser_path_mode = (uint32_t)momentum_settings.browser_path_mode;
        success &= flipper_format_write_uint32(
            file, "browser_path_mode", &browser_path_mode, 1);
        success &= flipper_format_write_bool(
            file, "dark_mode", &momentum_settings.dark_mode, 1);
        success &= flipper_format_write_bool(
            file,
            "file_naming_prefix_after",
            &momentum_settings.file_naming_prefix_after,
            1);
    }

    if(!success) {
        FURI_LOG_E(TAG, "Failed to save %s", MOMENTUM_SETTINGS_PATH);
    }

    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    return success;
}
