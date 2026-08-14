#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASSET_PACKS_NAME_LEN 32

#define MOMENTUM_ANIM_SPEED_MIN     25U
#define MOMENTUM_ANIM_SPEED_MAX     300U
#define MOMENTUM_ANIM_SPEED_DEFAULT 100U

#define MOMENTUM_CYCLE_ANIMS_MIN     (-1)
#define MOMENTUM_CYCLE_ANIMS_MAX     86400
#define MOMENTUM_CYCLE_ANIMS_DEFAULT 0

typedef enum {
    MenuStyleList = 0,
    MenuStyleWii,
    MenuStyleDsi,
    MenuStylePs4,
    MenuStyleVertical,
    MenuStyleC64,
    MenuStyleCompact,
    MenuStyleMNTM,
    MenuStyleCoverFlow,
    MenuStyleCount,
} MenuStyle;

typedef enum {
    BrowserPathOff = 0,
    BrowserPathCurrent,
    BrowserPathBrief,
    BrowserPathFull,
    BrowserPathModeCount,
} BrowserPathMode;

typedef struct {
    char asset_pack[ASSET_PACKS_NAME_LEN];
    uint32_t anim_speed;
    int32_t cycle_anims;
    bool unlock_anims;
    MenuStyle menu_style;
    bool lockscreen_poweroff;
    bool lockscreen_time;
    bool lockscreen_seconds;
    bool lockscreen_date;
    bool lockscreen_statusbar;
    bool lockscreen_prompt;
    bool lockscreen_transparent;
    bool lockscreen_skip_animation;
    bool scroll_marquee;
    bool midnight_format_00;
    bool popup_overlay;
    bool status_icons;
    bool bar_borders;
    bool bar_background;
    bool sort_dirs_first;
    bool show_hidden_files;
    bool show_internal_tab;
    BrowserPathMode browser_path_mode;
    bool dark_mode;
    bool file_naming_prefix_after;
} MomentumSettings;

/* Every persisted setting is described once, in momentum_settings_entries.
 * Defaults, clamping, change detection, loading and saving all iterate that
 * table, so adding a Momentum setting is one struct field plus one table row.
 * Fields are addressed by offset rather than by pointer so the same table can
 * be applied to any MomentumSettings instance, not just the global one. */
typedef enum {
    MomentumSettingsTypeStr,
    MomentumSettingsTypeInt,
    MomentumSettingsTypeUint,
    MomentumSettingsTypeBool,
    /* Serialised like Uint, but sanitized by falling back to its default
     * instead of clamping, since no enum case sits outside [min, max]. */
    MomentumSettingsTypeEnum,
} MomentumSettingsType;

/* Values are carried as int64_t so that the full uint32_t range and negative
 * int32_t values are both representable without one aliasing the other. */
typedef struct {
    MomentumSettingsType type;
    const char* key;
    uint16_t offset;
    uint16_t size;
    int64_t min;
    int64_t max;
    int64_t def;
} MomentumSettingsEntry;

/* Tripwire: adding a field to MomentumSettings without adding its table row
 * would leave it unsaved and invisible to change detection. Bump this only
 * together with the corresponding momentum_settings_entries row. */
#define MOMENTUM_SETTINGS_ENTRY_COUNT 25

extern const MomentumSettingsEntry momentum_settings_entries[];
extern const size_t momentum_settings_entry_count;

/* Read/write one table entry on an arbitrary settings instance. Strings are
 * not addressable this way; use the asset_pack field directly. */
int64_t momentum_settings_entry_get(const MomentumSettingsEntry* entry, const MomentumSettings* settings);
void momentum_settings_entry_set(
    const MomentumSettingsEntry* entry,
    MomentumSettings* settings,
    int64_t value);

void momentum_settings_set_defaults(MomentumSettings* settings);
void momentum_settings_sanitize(MomentumSettings* settings);
bool momentum_settings_equal(const MomentumSettings* left, const MomentumSettings* right);
bool momentum_settings_asset_pack_is_safe(const char* name);

uint32_t momentum_animation_frame_rate(uint32_t base_frame_rate, uint32_t speed_percent);
uint32_t momentum_animation_cycle_period_ms(
    int32_t cycle_seconds,
    uint32_t metadata_duration_seconds,
    bool* enabled);

#ifdef __cplusplus
}
#endif
