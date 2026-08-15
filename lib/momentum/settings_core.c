#include "settings_core.h"

#include <string.h>

#define FIELD_OFF(n) ((uint16_t)offsetof(MomentumSettings, n))
#define FIELD_SZ(n)  ((uint16_t)sizeof(((MomentumSettings*)0)->n))

#define ENTRY_STR(n)              {MomentumSettingsTypeStr, #n, FIELD_OFF(n), FIELD_SZ(n), 0, 0, 0}
#define ENTRY_BOOL(n, d)          {MomentumSettingsTypeBool, #n, FIELD_OFF(n), FIELD_SZ(n), 0, 1, (d)}
#define ENTRY_INT(n, lo, hi, d)   {MomentumSettingsTypeInt, #n, FIELD_OFF(n), FIELD_SZ(n), (lo), (hi), (d)}
#define ENTRY_UINT(n, lo, hi, d)  {MomentumSettingsTypeUint, #n, FIELD_OFF(n), FIELD_SZ(n), (lo), (hi), (d)}
#define ENTRY_ENUM(n, count, d)   {MomentumSettingsTypeEnum, #n, FIELD_OFF(n), FIELD_SZ(n), 0, (count) - 1, (d)}

const MomentumSettingsEntry momentum_settings_entries[] = {
    ENTRY_STR(asset_pack),
    ENTRY_UINT(
        anim_speed,
        MOMENTUM_ANIM_SPEED_MIN,
        MOMENTUM_ANIM_SPEED_MAX,
        MOMENTUM_ANIM_SPEED_DEFAULT),
    ENTRY_INT(
        cycle_anims,
        MOMENTUM_CYCLE_ANIMS_MIN,
        MOMENTUM_CYCLE_ANIMS_MAX,
        MOMENTUM_CYCLE_ANIMS_DEFAULT),
    ENTRY_BOOL(unlock_anims, false),
    ENTRY_ENUM(menu_style, MenuStyleCount, MenuStyleList),
    ENTRY_BOOL(lock_on_boot, true),
    ENTRY_BOOL(bad_pins_format, false),
    ENTRY_BOOL(allow_locked_rpc_usb, false),
    ENTRY_BOOL(allow_locked_rpc_ble, false),
    ENTRY_UINT(
        butthurt_timer,
        MOMENTUM_BUTTHURT_TIMER_MIN,
        MOMENTUM_BUTTHURT_TIMER_MAX,
        MOMENTUM_BUTTHURT_TIMER_DEFAULT),
    ENTRY_BOOL(lockscreen_poweroff, true),
    ENTRY_BOOL(lockscreen_time, true),
    ENTRY_BOOL(lockscreen_seconds, false),
    ENTRY_BOOL(lockscreen_date, true),
    ENTRY_BOOL(lockscreen_statusbar, true),
    ENTRY_BOOL(lockscreen_prompt, true),
    ENTRY_BOOL(lockscreen_transparent, false),
    ENTRY_BOOL(lockscreen_skip_animation, false),
    ENTRY_BOOL(scroll_marquee, false),
    ENTRY_BOOL(midnight_format_00, true),
    ENTRY_BOOL(popup_overlay, true),
    ENTRY_BOOL(status_icons, true),
    ENTRY_BOOL(bar_borders, true),
    ENTRY_BOOL(bar_background, false),
    ENTRY_BOOL(sort_dirs_first, true),
    ENTRY_BOOL(show_hidden_files, false),
    ENTRY_BOOL(show_internal_tab, false),
    ENTRY_ENUM(browser_path_mode, BrowserPathModeCount, BrowserPathOff),
    ENTRY_BOOL(dark_mode, false),
    ENTRY_BOOL(file_naming_prefix_after, false),
};

const size_t momentum_settings_entry_count =
    sizeof(momentum_settings_entries) / sizeof(momentum_settings_entries[0]);

/* Only MomentumSettingsTypeInt is signed. Widening every other type through
 * int32_t would turn a large uint32_t into a negative number, which would then
 * clamp to the entry's minimum instead of its maximum. */
static bool entry_is_signed(const MomentumSettingsEntry* entry) {
    return entry->type == MomentumSettingsTypeInt;
}

static void field_write(void* field, uint16_t size, bool is_signed, int64_t value) {
    if(size == 1) {
        uint8_t narrowed = (uint8_t)value;
        memcpy(field, &narrowed, sizeof(narrowed));
    } else if(size == 2) {
        uint16_t narrowed = (uint16_t)value;
        memcpy(field, &narrowed, sizeof(narrowed));
    } else if(is_signed) {
        int32_t narrowed = (int32_t)value;
        memcpy(field, &narrowed, sizeof(narrowed));
    } else {
        uint32_t narrowed = (uint32_t)value;
        memcpy(field, &narrowed, sizeof(narrowed));
    }
}

static int64_t field_read(const void* field, uint16_t size, bool is_signed) {
    if(size == 1) {
        uint8_t widened;
        memcpy(&widened, field, sizeof(widened));
        return (int64_t)widened;
    }
    if(size == 2) {
        uint16_t widened;
        memcpy(&widened, field, sizeof(widened));
        return (int64_t)widened;
    }
    if(is_signed) {
        int32_t widened;
        memcpy(&widened, field, sizeof(widened));
        return (int64_t)widened;
    }
    uint32_t widened;
    memcpy(&widened, field, sizeof(widened));
    return (int64_t)widened;
}

int64_t
    momentum_settings_entry_get(const MomentumSettingsEntry* entry, const MomentumSettings* settings) {
    if(!entry || !settings || entry->type == MomentumSettingsTypeStr) return 0;
    return field_read(
        (const uint8_t*)settings + entry->offset, entry->size, entry_is_signed(entry));
}

void momentum_settings_entry_set(
    const MomentumSettingsEntry* entry,
    MomentumSettings* settings,
    int64_t value) {
    if(!entry || !settings || entry->type == MomentumSettingsTypeStr) return;
    field_write((uint8_t*)settings + entry->offset, entry->size, entry_is_signed(entry), value);
}

void momentum_settings_set_defaults(MomentumSettings* settings) {
    if(!settings) return;

    memset(settings, 0, sizeof(*settings));
    for(size_t i = 0; i < momentum_settings_entry_count; ++i) {
        const MomentumSettingsEntry* entry = &momentum_settings_entries[i];
        if(entry->type == MomentumSettingsTypeStr) continue;
        momentum_settings_entry_set(entry, settings, entry->def);
    }
}

bool momentum_settings_asset_pack_is_safe(const char* name) {
    if(!name) return false;
    if(!strcmp(name, ".") || !strcmp(name, "..")) return false;

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

    for(size_t i = 0; i < momentum_settings_entry_count; ++i) {
        const MomentumSettingsEntry* entry = &momentum_settings_entries[i];
        if(entry->type == MomentumSettingsTypeStr) continue;

        const int64_t value = momentum_settings_entry_get(entry, settings);
        if(value >= entry->min && value <= entry->max) continue;

        /* An out-of-range numeric setting is still meaningful at the nearest
         * limit, but an out-of-range enum names no case at all, so it falls
         * back to its default rather than to a neighbouring style. */
        if(entry->type == MomentumSettingsTypeEnum) {
            momentum_settings_entry_set(entry, settings, entry->def);
        } else {
            momentum_settings_entry_set(entry, settings, value < entry->min ? entry->min : entry->max);
        }
    }
}

bool momentum_settings_equal(const MomentumSettings* left, const MomentumSettings* right) {
    if(!left || !right) return left == right;

    for(size_t i = 0; i < momentum_settings_entry_count; ++i) {
        const MomentumSettingsEntry* entry = &momentum_settings_entries[i];
        if(entry->type == MomentumSettingsTypeStr) {
            if(strncmp(
                   (const char*)left + entry->offset,
                   (const char*)right + entry->offset,
                   entry->size)) {
                return false;
            }
        } else if(
            momentum_settings_entry_get(entry, left) != momentum_settings_entry_get(entry, right)) {
            return false;
        }
    }

    return true;
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
