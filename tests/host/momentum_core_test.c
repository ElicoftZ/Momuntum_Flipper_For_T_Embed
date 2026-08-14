#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "momentum/settings_core.h"

static void test_defaults_and_sanitizing(void) {
    MomentumSettings settings;
    memset(&settings, 0xA5, sizeof(settings));
    momentum_settings_set_defaults(&settings);

    assert(settings.asset_pack[0] == '\0');
    assert(settings.anim_speed == 100U);
    assert(settings.cycle_anims == 0);
    assert(!settings.unlock_anims);
    assert(settings.menu_style == MenuStyleList);
    assert(settings.lockscreen_poweroff);
    assert(settings.lockscreen_time);
    assert(!settings.lockscreen_seconds);
    assert(settings.lockscreen_date);
    assert(settings.lockscreen_statusbar);
    assert(settings.lockscreen_prompt);
    assert(!settings.lockscreen_transparent);
    assert(!settings.lockscreen_skip_animation);
    assert(!settings.scroll_marquee);
    assert(settings.midnight_format_00);
    assert(settings.popup_overlay);
    assert(settings.status_icons);
    assert(settings.bar_borders);
    assert(!settings.bar_background);
    assert(settings.sort_dirs_first);
    assert(!settings.show_hidden_files);
    assert(!settings.show_internal_tab);
    assert(settings.browser_path_mode == BrowserPathOff);
    assert(!settings.dark_mode);
    assert(!settings.file_naming_prefix_after);

    memset(settings.asset_pack, 'A', sizeof(settings.asset_pack));
    settings.anim_speed = 0;
    settings.cycle_anims = -200;
    settings.menu_style = (MenuStyle)-1;
    settings.browser_path_mode = (BrowserPathMode)-1;
    momentum_settings_sanitize(&settings);
    assert(settings.asset_pack[ASSET_PACKS_NAME_LEN - 1] == '\0');
    assert(settings.anim_speed == MOMENTUM_ANIM_SPEED_MIN);
    assert(settings.cycle_anims == MOMENTUM_CYCLE_ANIMS_MIN);
    assert(settings.menu_style == MenuStyleList);
    assert(settings.browser_path_mode == BrowserPathOff);

    settings.anim_speed = UINT32_MAX;
    settings.cycle_anims = INT32_MAX;
    settings.menu_style = MenuStyleCount;
    settings.browser_path_mode = BrowserPathModeCount;
    strcpy(settings.asset_pack, "../escape");
    momentum_settings_sanitize(&settings);
    assert(settings.asset_pack[0] == '\0');
    assert(settings.anim_speed == MOMENTUM_ANIM_SPEED_MAX);
    assert(settings.cycle_anims == MOMENTUM_CYCLE_ANIMS_MAX);
    assert(settings.menu_style == MenuStyleList);
    assert(settings.browser_path_mode == BrowserPathOff);
}

static void test_pack_names(void) {
    assert(momentum_settings_asset_pack_is_safe(""));
    assert(momentum_settings_asset_pack_is_safe("Momentum"));
    assert(momentum_settings_asset_pack_is_safe("Watch Dogs 2.0"));
    assert(!momentum_settings_asset_pack_is_safe(NULL));
    assert(!momentum_settings_asset_pack_is_safe("."));
    assert(!momentum_settings_asset_pack_is_safe(".."));
    assert(!momentum_settings_asset_pack_is_safe("../Momentum"));
    assert(!momentum_settings_asset_pack_is_safe("folder\\Momentum"));
    assert(!momentum_settings_asset_pack_is_safe("bad\nname"));
}

static void test_animation_math(void) {
    assert(momentum_animation_frame_rate(4U, 25U) == 1U);
    assert(momentum_animation_frame_rate(4U, 100U) == 4U);
    assert(momentum_animation_frame_rate(4U, 300U) == 12U);
    assert(momentum_animation_frame_rate(1U, 25U) == 1U);
    assert(momentum_animation_frame_rate(UINT32_MAX, 300U) == UINT32_MAX);

    bool enabled = true;
    assert(momentum_animation_cycle_period_ms(-1, 30U, &enabled) == 0U);
    assert(!enabled);

    assert(momentum_animation_cycle_period_ms(0, 30U, &enabled) == 30000U);
    assert(enabled);

    assert(momentum_animation_cycle_period_ms(15, 30U, &enabled) == 15000U);
    assert(enabled);

    assert(momentum_animation_cycle_period_ms(0, 0U, &enabled) == 0U);
    assert(!enabled);

    assert(momentum_animation_cycle_period_ms(0, UINT32_MAX, &enabled) == UINT32_MAX);
    assert(enabled);
}

int main(void) {
    test_defaults_and_sanitizing();
    test_pack_names();
    test_animation_math();
    puts("Momentum core host tests passed.");
    return 0;
}
