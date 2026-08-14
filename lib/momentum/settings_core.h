#pragma once

#include <stdbool.h>
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

void momentum_settings_set_defaults(MomentumSettings* settings);
void momentum_settings_sanitize(MomentumSettings* settings);
bool momentum_settings_asset_pack_is_safe(const char* name);

uint32_t momentum_animation_frame_rate(uint32_t base_frame_rate, uint32_t speed_percent);
uint32_t momentum_animation_cycle_period_ms(
    int32_t cycle_seconds,
    uint32_t metadata_duration_seconds,
    bool* enabled);

#ifdef __cplusplus
}
#endif
