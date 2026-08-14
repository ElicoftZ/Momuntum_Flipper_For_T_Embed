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

static const MomentumSettingsEntry* entry_named(const char* key) {
    for(size_t i = 0; i < momentum_settings_entry_count; i++) {
        if(!strcmp(momentum_settings_entries[i].key, key)) return &momentum_settings_entries[i];
    }
    assert(!"no settings entry with that key");
    return NULL;
}

/* The settings table is the single source of truth for defaults, clamping,
 * change detection, loading and saving, so a malformed or incomplete table
 * silently breaks all five at once. */
static void test_entry_table_is_well_formed(void) {
    assert(momentum_settings_entry_count == MOMENTUM_SETTINGS_ENTRY_COUNT);

    for(size_t i = 0; i < momentum_settings_entry_count; i++) {
        const MomentumSettingsEntry* entry = &momentum_settings_entries[i];

        assert(entry->key != NULL);
        assert(entry->key[0] != '\0');
        assert(entry->size > 0);
        assert((size_t)entry->offset + entry->size <= sizeof(MomentumSettings));

        if(entry->type != MomentumSettingsTypeStr) {
            assert(entry->min <= entry->max);
            assert(entry->def >= entry->min && entry->def <= entry->max);
        }

        for(size_t j = i + 1; j < momentum_settings_entry_count; j++) {
            const MomentumSettingsEntry* other = &momentum_settings_entries[j];
            assert(strcmp(entry->key, other->key) != 0);
            /* Overlapping entries would make two keys fight over one field. */
            assert(
                entry->offset + entry->size <= other->offset ||
                other->offset + other->size <= entry->offset);
        }
    }
}

/* Guards the change detection that decides whether the desktop reloads and
 * whether a save is needed: every entry must be compared, not just the ones
 * someone remembered to list. */
static void test_equal_covers_every_entry(void) {
    MomentumSettings a;
    MomentumSettings b;
    momentum_settings_set_defaults(&a);
    momentum_settings_set_defaults(&b);
    assert(momentum_settings_equal(&a, &b));

    strcpy(a.asset_pack, "Momentum");
    assert(!momentum_settings_equal(&a, &b));
    strcpy(a.asset_pack, "");
    assert(momentum_settings_equal(&a, &b));

    for(size_t i = 0; i < momentum_settings_entry_count; i++) {
        const MomentumSettingsEntry* entry = &momentum_settings_entries[i];
        if(entry->type == MomentumSettingsTypeStr) continue;

        const int64_t original = momentum_settings_entry_get(entry, &a);
        const int64_t altered = original == entry->max ? entry->min : entry->max;
        assert(altered != original);

        momentum_settings_entry_set(entry, &a, altered);
        assert(!momentum_settings_equal(&a, &b));

        momentum_settings_entry_set(entry, &a, original);
        assert(momentum_settings_equal(&a, &b));
    }
}

/* A uint32_t setting read through a signed 32-bit value would come back
 * negative and clamp to its minimum instead of its maximum. */
static void test_unsigned_entries_do_not_wrap(void) {
    MomentumSettings settings;
    momentum_settings_set_defaults(&settings);

    settings.anim_speed = UINT32_MAX;
    assert(momentum_settings_entry_get(entry_named("anim_speed"), &settings) == UINT32_MAX);

    momentum_settings_sanitize(&settings);
    assert(settings.anim_speed == MOMENTUM_ANIM_SPEED_MAX);

    settings.cycle_anims = -5;
    assert(momentum_settings_entry_get(entry_named("cycle_anims"), &settings) == -5);
}

int main(void) {
    test_defaults_and_sanitizing();
    test_pack_names();
    test_animation_math();
    test_entry_table_is_well_formed();
    test_equal_covers_every_entry();
    test_unsigned_entries_do_not_wrap();
    puts("Momentum core host tests passed.");
    return 0;
}
