#include "settings_core.h"

static bool momentum_strings_equal(const char* left, const char* right) {
    while(*left && (*left == *right)) {
        ++left;
        ++right;
    }
    return *left == *right;
}

void momentum_settings_set_defaults(MomentumSettings* settings) {
    if(!settings) return;

    uint8_t* bytes = (uint8_t*)settings;
    for(uint32_t i = 0; i < sizeof(*settings); ++i) {
        bytes[i] = 0;
    }
    settings->anim_speed = MOMENTUM_ANIM_SPEED_DEFAULT;
    settings->cycle_anims = MOMENTUM_CYCLE_ANIMS_DEFAULT;
    settings->unlock_anims = false;
    settings->menu_style = MenuStyleList;
    settings->lockscreen_poweroff = true;
    settings->lockscreen_time = true;
    settings->lockscreen_seconds = false;
    settings->lockscreen_date = true;
    settings->lockscreen_statusbar = true;
    settings->lockscreen_prompt = true;
    settings->lockscreen_transparent = false;
    settings->lockscreen_skip_animation = false;
    settings->scroll_marquee = false;
    settings->midnight_format_00 = true;
    settings->popup_overlay = true;
    settings->status_icons = true;
    settings->bar_borders = true;
    settings->bar_background = false;
    settings->sort_dirs_first = true;
    settings->show_hidden_files = false;
    settings->show_internal_tab = false;
    settings->browser_path_mode = BrowserPathOff;
    settings->dark_mode = false;
    settings->file_naming_prefix_after = false;
}

bool momentum_settings_asset_pack_is_safe(const char* name) {
    if(!name) return false;
    if(momentum_strings_equal(name, ".") || momentum_strings_equal(name, "..")) return false;

    for(const unsigned char* cursor = (const unsigned char*)name; *cursor; ++cursor) {
        if((*cursor < 0x20U) || (*cursor == 0x7FU) || (*cursor == '/') || (*cursor == '\\')) {
            return false;
        }
    }

    return true;
}

void momentum_settings_sanitize(MomentumSettings* settings) {
    if(!settings) return;

    settings->asset_pack[ASSET_PACKS_NAME_LEN - 1] = '\0';
    if(!momentum_settings_asset_pack_is_safe(settings->asset_pack)) {
        settings->asset_pack[0] = '\0';
    }

    if(settings->anim_speed < MOMENTUM_ANIM_SPEED_MIN) {
        settings->anim_speed = MOMENTUM_ANIM_SPEED_MIN;
    } else if(settings->anim_speed > MOMENTUM_ANIM_SPEED_MAX) {
        settings->anim_speed = MOMENTUM_ANIM_SPEED_MAX;
    }

    if(settings->cycle_anims < MOMENTUM_CYCLE_ANIMS_MIN) {
        settings->cycle_anims = MOMENTUM_CYCLE_ANIMS_MIN;
    } else if(settings->cycle_anims > MOMENTUM_CYCLE_ANIMS_MAX) {
        settings->cycle_anims = MOMENTUM_CYCLE_ANIMS_MAX;
    }

    if(((int32_t)settings->menu_style < 0) || (settings->menu_style >= MenuStyleCount)) {
        settings->menu_style = MenuStyleList;
    }

    if(((int32_t)settings->browser_path_mode < 0) ||
       (settings->browser_path_mode >= BrowserPathModeCount)) {
        settings->browser_path_mode = BrowserPathOff;
    }
}

uint32_t momentum_animation_frame_rate(uint32_t base_frame_rate, uint32_t speed_percent) {
    if(speed_percent < MOMENTUM_ANIM_SPEED_MIN) {
        speed_percent = MOMENTUM_ANIM_SPEED_MIN;
    } else if(speed_percent > MOMENTUM_ANIM_SPEED_MAX) {
        speed_percent = MOMENTUM_ANIM_SPEED_MAX;
    }

    uint64_t scaled = ((uint64_t)base_frame_rate * speed_percent) / 100U;
    if(scaled < 1U) return 1U;
    if(scaled > UINT32_MAX) return UINT32_MAX;
    return (uint32_t)scaled;
}

uint32_t momentum_animation_cycle_period_ms(
    int32_t cycle_seconds,
    uint32_t metadata_duration_seconds,
    bool* enabled) {
    if(enabled) *enabled = false;
    if(cycle_seconds < 0) return 0;

    const uint32_t seconds = cycle_seconds == 0 ? metadata_duration_seconds : (uint32_t)cycle_seconds;
    if(seconds == 0) return 0;

    if(enabled) *enabled = true;
    const uint64_t milliseconds = (uint64_t)seconds * 1000U;
    return milliseconds > UINT32_MAX ? UINT32_MAX : (uint32_t)milliseconds;
}
