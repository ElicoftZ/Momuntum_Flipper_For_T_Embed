#include <furi.h>
#include <furi_hal.h>
#include <applications.h>
#include <assets_icons.h>
#include <desktop/desktop.h>
#include <desktop/views/desktop_view_slideshow.h>
#include <dialogs/dialogs.h>
#include <dolphin/dolphin.h>
#include <flipper_application/flipper_application.h>
#include <dolphin/dolphin_i.h>
#include <dolphin/helpers/dolphin_state.h>
#include <gui/gui.h>
#include <gui/modules/submenu.h>
#include <gui/modules/number_input.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <flipper_format/flipper_format.h>
#include <gui/view_dispatcher.h>
/* Resolves to components/loader, which is the loader that this build compiles;
 * applications/services/loader is a second, uncompiled copy. */
#include <loader/loader_menu.h>
#include <momentum/momentum.h>
#include <namechanger/namechanger.h>
#include <power/power_service/power.h>
#include <storage/storage.h>
#include <subghz/subghz_extended_range.h>
#include <toolbox/stream/file_stream.h>
#include <toolbox/value_index.h>
#include <toolbox/name_generator.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAG "MomentumSettingsApp"
#define MOMENTUM_MAX_SELECTABLE_PACKS 254U
#define MOMENTUM_DIRECTORY_NAME_SIZE  256U
#define MOMENTUM_MAX_USER_FREQS       24U
#define MOMENTUM_MAX_MAINMENU_ITEMS   64U
#define MOMENTUM_DOLPHIN_XP_MAX       9999U
#define MOMENTUM_DOLPHIN_BUTTHURT_MAX 14
/* Embedded in the firmware image (main/CMakeLists.txt EMBED_FILES), so the
 * intro works on a card that has never seen this firmware. A copy on the SD
 * card still wins when present, which keeps the slideshow replaceable
 * without a rebuild. */
extern const uint8_t firstboot_bin_start[] asm("_binary_firstboot_bin_start");
extern const uint8_t firstboot_bin_end[] asm("_binary_firstboot_bin_end");

#define MOMENTUM_INTRO_PATH           EXT_PATH("dolphin/firstboot.bin")
#define MOMENTUM_SUBGHZ_USER_SETTINGS EXT_PATH("subghz/assets/setting_user")
/* Mirrors the private constants in lib/subghz/subghz_setting.c, which are not
 * exported by its header. They must stay in step with it. */
#define MOMENTUM_SUBGHZ_SETTING_TYPE    "Flipper SubGhz Setting File"
#define MOMENTUM_SUBGHZ_SETTING_VERSION 1

typedef enum {
    MomentumSettingsViewList,
    MomentumSettingsViewAssetPacks,
    MomentumSettingsViewTextInput,
    MomentumSettingsViewFreqList,
    MomentumSettingsViewNumberInput,
    MomentumSettingsViewMainmenu,
} MomentumSettingsView;

typedef enum {
    MomentumSettingsPageRoot,
    MomentumSettingsPageInterface,
    MomentumSettingsPageGraphics,
    MomentumSettingsPageMainmenu,
    MomentumSettingsPageLockscreen,
    MomentumSettingsPageStatusbar,
    MomentumSettingsPageFileBrowser,
    MomentumSettingsPageGeneral,
    MomentumSettingsPageProtocols,
    MomentumSettingsPageMisc,
    MomentumSettingsPageScreen,
    MomentumSettingsPageFrequencies,
    MomentumSettingsPageDolphin,
    MomentumSettingsPageCount,
} MomentumSettingsPage;

/* The Mainmenu page borrows one Submenu for three screens, the way the
 * frequency list does, rather than allocating a view per screen. */
typedef enum {
    MomentumMainmenuScreenAddSource,
    MomentumMainmenuScreenAddBuiltin,
    MomentumMainmenuScreenResetConfirm,
} MomentumMainmenuScreen;

/* Rows of the Mainmenu page, which the enter handler and the post-edit refresh
 * both index into. */
typedef enum {
    MomentumMainmenuRowMenuStyle,
    MomentumMainmenuRowReset,
    MomentumMainmenuRowItem,
    MomentumMainmenuRowAdd,
    MomentumMainmenuRowMove,
    MomentumMainmenuRowRemove,
    MomentumMainmenuRowHideDualBoot,
} MomentumMainmenuRow;

typedef struct {
    Gui* gui;
    Storage* storage;
    Desktop* desktop;
    Power* power;
    Dolphin* dolphin;
    DialogsApp* dialogs;
    ViewDispatcher* view_dispatcher;
    VariableItemList* variable_item_list;
    Submenu* asset_pack_submenu;
    Submenu* freq_submenu;
    Submenu* mainmenu_submenu;
    TextInput* text_input;
    NumberInput* number_input;
    VariableItem* asset_pack_item;
    char** asset_pack_names;
    uint8_t asset_pack_count;
    MomentumSettings settings;
    DesktopSettings desktop_settings;
    MomentumSettingsPage current_page;
    bool sd_ready;
    bool dirty;
    bool desktop_dirty;
    bool name_dirty;
    bool subghz_extended_range;
    bool subghz_use_defaults;
    bool subghz_freqs_dirty;
    bool subghz_editing_hopper;
    bool dolphin_dirty;
    bool number_input_for_xp;
    uint32_t dolphin_xp;
    int32_t dolphin_butthurt;
    uint8_t subghz_static_count;
    uint8_t subghz_hopper_count;
    uint32_t subghz_static_freqs[MOMENTUM_MAX_USER_FREQS];
    uint32_t subghz_hopper_freqs[MOMENTUM_MAX_USER_FREQS];
    char device_name[FURI_HAL_VERSION_ARRAY_NAME_LENGTH];
    /* labels are what the page shows, lines are what gets written back. They
     * differ only for file entries, where the label is the FAP manifest name
     * and the line is the path. */
    char* mainmenu_labels[MOMENTUM_MAX_MAINMENU_ITEMS];
    char* mainmenu_lines[MOMENTUM_MAX_MAINMENU_ITEMS];
    uint8_t mainmenu_count;
    uint8_t mainmenu_index;
    bool mainmenu_dirty;
    bool reboot_for_intro;
    MomentumMainmenuScreen mainmenu_screen;
    /* variable_item_set_item_label() keeps the pointer it is given rather than
     * copying, so the "Item n/m" text has to outlive the call. */
    char mainmenu_item_label[24];
} MomentumSettingsApp;

static const uint32_t momentum_anim_speed_values[] = {
    25,
    50,
    75,
    100,
    125,
    150,
    175,
    200,
    225,
    250,
    275,
    300,
};

static const char* const momentum_anim_speed_text[] = {
    "25%",
    "50%",
    "75%",
    "100%",
    "125%",
    "150%",
    "175%",
    "200%",
    "225%",
    "250%",
    "275%",
    "300%",
};

static const int32_t momentum_cycle_anim_values[] = {
    -1,
    0,
    15,
    30,
    60,
    120,
    300,
    600,
    900,
    1800,
    3600,
    7200,
    21600,
    43200,
    86400,
};

static const char* const momentum_cycle_anim_text[] = {
    "OFF",
    "Meta.txt",
    "15 S",
    "30 S",
    "1 M",
    "2 M",
    "5 M",
    "10 M",
    "15 M",
    "30 M",
    "1 H",
    "2 H",
    "6 H",
    "12 H",
    "24 H",
};

static const bool momentum_unlock_anim_values[] = {
    false,
    true,
};

static const char* const momentum_unlock_anim_text[] = {
    "OFF",
    "ON",
};

static const char* const momentum_menu_style_text[MenuStyleCount] = {
    "List",
    "Wii",
    "DSi",
    "PS4",
    "Vertical",
    "C64",
    "Compact",
    "MNTM",
    "CoverFlow",
};

static const char* const momentum_browser_path_text[] = {
    "OFF",
    "Current",
    "Brief",
    "Full",
};

static const uint32_t momentum_battery_icon_values[] = {
    DISPLAY_BATTERY_OFF,
    DISPLAY_BATTERY_BAR,
    DISPLAY_BATTERY_PERCENT,
    DISPLAY_BATTERY_INVERTED_PERCENT,
    DISPLAY_BATTERY_RETRO_3,
    DISPLAY_BATTERY_RETRO_5,
    DISPLAY_BATTERY_BAR_PERCENT,
};

static const char* const momentum_battery_icon_text[] = {
    "OFF",
    "Bar",
    "%",
    "Inv. %",
    "Retro 3",
    "Retro 5",
    "Bar %",
};

static const uint32_t momentum_butthurt_timer_values[] = {
    0, 1800, 3600, 7200, 14400, 21600, 28800, 43200, 86400, 172800,
};

static const char* const momentum_butthurt_timer_text[] = {
    "OFF", "30 M", "1 H", "2 H", "4 H", "6 H", "8 H", "12 H", "24 H", "48 H",
};

static const uint32_t momentum_clock_values[] = {
    0,
    1,
};

static void momentum_settings_show_page(
    MomentumSettingsApp* app,
    MomentumSettingsPage page,
    uint8_t selected_item);
static void momentum_settings_show_freq_list(MomentumSettingsApp* app);
static void momentum_settings_use_defaults_changed(VariableItem* item);
static void momentum_settings_butthurt_changed(VariableItem* item);
static void momentum_settings_number_done(void* context, int32_t number);
static void momentum_settings_mainmenu_show_reset(MomentumSettingsApp* app);
static void momentum_settings_mainmenu_show_add_source(MomentumSettingsApp* app);
static void momentum_settings_mainmenu_remove(MomentumSettingsApp* app, uint8_t index);
static void momentum_settings_mainmenu_refresh(MomentumSettingsApp* app);

static bool momentum_settings_add_asset_pack(MomentumSettingsApp* app, const char* name) {
    furi_assert(app);
    furi_assert(name);

    if(app->asset_pack_count >= MOMENTUM_MAX_SELECTABLE_PACKS) return false;

    const size_t name_length = strlen(name);
    char* name_copy = malloc(name_length + 1U);
    if(!name_copy) return false;
    memcpy(name_copy, name, name_length + 1U);

    char** resized_names =
        realloc(app->asset_pack_names, (app->asset_pack_count + 1U) * sizeof(char*));
    if(!resized_names) {
        free(name_copy);
        return false;
    }
    app->asset_pack_names = resized_names;

    uint8_t insert_at = 0;
    while(insert_at < app->asset_pack_count) {
        int comparison = strcasecmp(app->asset_pack_names[insert_at], name);
        if(comparison == 0) comparison = strcmp(app->asset_pack_names[insert_at], name);
        if(comparison > 0) break;
        insert_at++;
    }

    memmove(
        &app->asset_pack_names[insert_at + 1U],
        &app->asset_pack_names[insert_at],
        (app->asset_pack_count - insert_at) * sizeof(char*));
    app->asset_pack_names[insert_at] = name_copy;
    app->asset_pack_count++;
    return true;
}

static void momentum_settings_scan_asset_packs(MomentumSettingsApp* app) {
    furi_assert(app);
    if(!app->sd_ready) return;

    File* directory = storage_file_alloc(app->storage);
    bool opened = storage_dir_open(directory, ASSET_PACKS_PATH);
    if(opened) {
        FileInfo file_info;
        char name[MOMENTUM_DIRECTORY_NAME_SIZE];

        while(storage_dir_read(directory, &file_info, name, sizeof(name))) {
            const size_t name_length = strlen(name);
            if(!file_info_is_dir(&file_info) || name_length == 0 || name[0] == '.' ||
               name_length >= ASSET_PACKS_NAME_LEN ||
               !momentum_settings_asset_pack_is_safe(name)) {
                continue;
            }

            char manifest_path[MOMENTUM_DIRECTORY_NAME_SIZE];
            int written = snprintf(
                manifest_path, sizeof(manifest_path), "%s/%s/Anims/manifest.txt", ASSET_PACKS_PATH, name);
            if(written <= 0 || (size_t)written >= sizeof(manifest_path) ||
               !storage_file_exists(app->storage, manifest_path)) {
                continue;
            }

            if(!momentum_settings_add_asset_pack(app, name)) {
                FURI_LOG_W(TAG, "Asset pack limit reached while scanning %s", ASSET_PACKS_PATH);
                break;
            }
        }
    }

    storage_dir_close(directory);
    storage_file_free(directory);
}

static uint8_t momentum_settings_asset_pack_index(const MomentumSettingsApp* app) {
    furi_assert(app);
    if(app->settings.asset_pack[0] == '\0') return 0;

    for(uint8_t i = 0; i < app->asset_pack_count; i++) {
        if(!strcmp(app->asset_pack_names[i], app->settings.asset_pack)) return i + 1U;
    }

    return 0;
}

static void momentum_settings_update_asset_pack_item(MomentumSettingsApp* app) {
    furi_assert(app);
    variable_item_set_current_value_text(
        app->asset_pack_item,
        app->settings.asset_pack[0] ? app->settings.asset_pack : "Default");
}

static void momentum_settings_asset_pack_selected(void* context, uint32_t index) {
    MomentumSettingsApp* app = context;
    furi_assert(app);
    furi_assert(index <= app->asset_pack_count);

    const char* selected_name = index ? app->asset_pack_names[index - 1U] : "";
    if(strcmp(app->settings.asset_pack, selected_name)) {
        snprintf(
            app->settings.asset_pack,
            sizeof(app->settings.asset_pack),
            "%s",
            selected_name);
        app->dirty = true;
        momentum_settings_update_asset_pack_item(app);
    }

    submenu_set_selected_item(app->asset_pack_submenu, index);
    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumSettingsViewList);
}

static void momentum_settings_device_name_done(void* context) {
    MomentumSettingsApp* app = context;
    app->name_dirty = true;
    momentum_settings_show_page(app, MomentumSettingsPageMisc, 1);
}

static void momentum_settings_list_enter(void* context, uint32_t index) {
    MomentumSettingsApp* app = context;
    furi_assert(app);

    if(app->current_page == MomentumSettingsPageRoot) {
        if(index == 0U) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, MomentumSettingsPageInterface);
        } else if(index == 1U) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, MomentumSettingsPageProtocols);
        } else if(index == 2U) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, MomentumSettingsPageMisc);
        }
    } else if(app->current_page == MomentumSettingsPageInterface) {
        if(index == 0U) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, MomentumSettingsPageGraphics);
        } else if(index == 1U) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, MomentumSettingsPageMainmenu);
        } else if(index == 2U) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, MomentumSettingsPageLockscreen);
        } else if(index == 3U) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, MomentumSettingsPageStatusbar);
        } else if(index == 4U) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, MomentumSettingsPageFileBrowser);
        } else if(index == 5U) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, MomentumSettingsPageGeneral);
        }
    } else if(app->current_page == MomentumSettingsPageMainmenu) {
        if(index == MomentumMainmenuRowReset) {
            momentum_settings_mainmenu_show_reset(app);
        } else if(index == MomentumMainmenuRowAdd) {
            momentum_settings_mainmenu_show_add_source(app);
        } else if(index == MomentumMainmenuRowRemove && app->mainmenu_count) {
            momentum_settings_mainmenu_remove(app, app->mainmenu_index);
            app->mainmenu_dirty = true;
            momentum_settings_mainmenu_refresh(app);
        }
    } else if(app->current_page == MomentumSettingsPageGraphics && index == 0U) {
        submenu_set_selected_item(
            app->asset_pack_submenu, momentum_settings_asset_pack_index(app));
        view_dispatcher_switch_to_view(
            app->view_dispatcher, MomentumSettingsViewAssetPacks);
    } else if(app->current_page == MomentumSettingsPageProtocols && index == 2U) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, MomentumSettingsPageFrequencies);
    } else if(app->current_page == MomentumSettingsPageDolphin && index == 1U) {
        app->number_input_for_xp = true;
        number_input_set_header_text(app->number_input, "Dolphin XP");
        number_input_set_result_callback(
            app->number_input,
            momentum_settings_number_done,
            app,
            (int32_t)app->dolphin_xp,
            0,
            (int32_t)MOMENTUM_DOLPHIN_XP_MAX);
        view_dispatcher_switch_to_view(app->view_dispatcher, MomentumSettingsViewNumberInput);
    } else if(app->current_page == MomentumSettingsPageFrequencies && index > 0U) {
        app->subghz_editing_hopper = (index == 2U);
        momentum_settings_show_freq_list(app);
    } else if(app->current_page == MomentumSettingsPageMisc) {
        if(index == 0U) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, MomentumSettingsPageScreen);
        } else if(index == 3U) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, MomentumSettingsPageDolphin);
        } else if(index == 4U) {
            /* The desktop only looks for the slideshow while it is starting up,
             * so the intro needs a reboot to play. Stopping the dispatcher
             * rather than rebooting here lets the normal exit path save
             * everything else the user changed first. */
            bool staged = false;

            /* A slideshow the user dropped on the card wins over the built-in
               one, so this stays customisable. */
            if(storage_file_exists(app->storage, MOMENTUM_INTRO_PATH)) {
                storage_common_remove(app->storage, SLIDESHOW_FS_PATH);
                staged = storage_common_copy(
                             app->storage, MOMENTUM_INTRO_PATH, SLIDESHOW_FS_PATH) == FSE_OK;
            }

            if(!staged) {
                const size_t len = (size_t)(firstboot_bin_end - firstboot_bin_start);
                File* out = storage_file_alloc(app->storage);
                if(storage_file_open(out, SLIDESHOW_FS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
                    staged = storage_file_write(out, firstboot_bin_start, (uint16_t)len) == len;
                }
                storage_file_close(out);
                storage_file_free(out);
            }

            if(staged) {
                app->reboot_for_intro = true;
                view_dispatcher_stop(app->view_dispatcher);
            } else {
                FURI_LOG_E(TAG, "could not stage the Momentum intro");
            }
        } else if(index == 1U) {
            text_input_set_header_text(app->text_input, "Device Name (empty = default)");
            text_input_set_result_callback(
                app->text_input,
                momentum_settings_device_name_done,
                app,
                app->device_name,
                sizeof(app->device_name),
                true);
            view_dispatcher_switch_to_view(
                app->view_dispatcher, MomentumSettingsViewTextInput);
        }
    }
}

static void momentum_settings_anim_speed_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_anim_speed_text[index]);
    app->settings.anim_speed = momentum_anim_speed_values[index];
    app->dirty = true;
}

static void momentum_settings_cycle_anims_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_cycle_anim_text[index]);
    app->settings.cycle_anims = momentum_cycle_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_hide_dualboot_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    /* Stored inverted from the label: the row reads "Dual Boot: Show/Hide",
     * the setting is hide_dualboot. */
    app->settings.hide_dualboot = (index != 0);
    app->dirty = true;
}

static void momentum_settings_unlock_anims_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.unlock_anims = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_lockscreen_poweroff_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.lockscreen_poweroff = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_lockscreen_time_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.lockscreen_time = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_lockscreen_seconds_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.lockscreen_seconds = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_lockscreen_date_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.lockscreen_date = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_lockscreen_statusbar_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.lockscreen_statusbar = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_lockscreen_prompt_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.lockscreen_prompt = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_lockscreen_transparent_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.lockscreen_transparent = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_lockscreen_skip_animation_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.lockscreen_skip_animation = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_battery_icon_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_battery_icon_text[index]);
    app->desktop_settings.displayBatteryPercentage = momentum_battery_icon_values[index];
    app->desktop_dirty = true;
}

static void momentum_settings_clock_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->desktop_settings.display_clock = momentum_clock_values[index];
    app->desktop_dirty = true;
}

static void momentum_settings_status_icons_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.status_icons = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_bar_borders_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.bar_borders = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_bar_background_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.bar_background = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_sort_dirs_first_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.sort_dirs_first = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_show_hidden_files_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.show_hidden_files = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_show_internal_tab_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.show_internal_tab = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_menu_style_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_menu_style_text[index]);
    app->settings.menu_style = (MenuStyle)index;
    // Apply live so the main menu already uses the new style on Back.
    momentum_settings.menu_style = app->settings.menu_style;
    app->dirty = true;
}

static void momentum_settings_lock_on_boot_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.lock_on_boot = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_locked_rpc_usb_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.allow_locked_rpc_usb = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_locked_rpc_ble_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.allow_locked_rpc_ble = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_butthurt_timer_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_butthurt_timer_text[index]);
    app->settings.butthurt_timer = momentum_butthurt_timer_values[index];
    // Applied live so the dolphin timer picks it up when this app exits.
    momentum_settings.butthurt_timer = app->settings.butthurt_timer;
    app->dirty = true;
}

static void momentum_settings_subghz_extended_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->subghz_extended_range = momentum_unlock_anim_values[index];
    /* Written and applied straight away rather than on exit: this changes what
     * the radio is allowed to transmit, so it should not linger unsaved. */
    subghz_extended_range_save(app->subghz_extended_range);
}

static void momentum_settings_browser_path_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_browser_path_text[index]);
    app->settings.browser_path_mode = (BrowserPathMode)index;
    app->dirty = true;
}

static void momentum_settings_dark_mode_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.dark_mode = momentum_unlock_anim_values[index];
    momentum_settings.dark_mode = app->settings.dark_mode;
    app->dirty = true;
}

static void momentum_settings_left_handed_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    if(momentum_unlock_anim_values[index]) {
        furi_hal_rtc_set_flag(FuriHalRtcFlagHandOrient);
    } else {
        furi_hal_rtc_reset_flag(FuriHalRtcFlagHandOrient);
    }
}

static void momentum_settings_file_naming_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, index ? "After" : "Before");
    app->settings.file_naming_prefix_after = index != 0U;
    app->dirty = true;
}

static void momentum_settings_scroll_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, index ? "Marquee" : "Standard");
    app->settings.scroll_marquee = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_midnight_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, index ? "00:XX" : "12:XX");
    app->settings.midnight_format_00 = momentum_unlock_anim_values[index];
    app->dirty = true;
}

static void momentum_settings_popup_overlay_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->settings.popup_overlay = momentum_unlock_anim_values[index];
    app->dirty = true;
}

/* ---- Main menu layout ---------------------------------------------------- */

static bool
    momentum_settings_mainmenu_push(MomentumSettingsApp* app, const char* label, const char* line) {
    if(app->mainmenu_count >= MOMENTUM_MAX_MAINMENU_ITEMS) return false;

    char* label_copy = strdup(label);
    char* line_copy = strdup(line);
    if(!label_copy || !line_copy) {
        free(label_copy);
        free(line_copy);
        return false;
    }

    app->mainmenu_labels[app->mainmenu_count] = label_copy;
    app->mainmenu_lines[app->mainmenu_count] = line_copy;
    app->mainmenu_count++;
    return true;
}

/* The pinned app is always in the menu and is not part of the editable list,
 * so it is skipped wherever the built-in apps are enumerated. */
static bool momentum_settings_mainmenu_is_pinned(size_t index) {
    return !strcmp(FLIPPER_EXTERNAL_APPS[index].path, MAINMENU_PINNED_APPID);
}

static void momentum_settings_mainmenu_load_defaults(MomentumSettingsApp* app) {
    for(size_t i = 0; i < FLIPPER_APPS_COUNT; i++) {
        momentum_settings_mainmenu_push(app, FLIPPER_APPS[i].name, FLIPPER_APPS[i].name);
    }
    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; i++) {
        if(momentum_settings_mainmenu_is_pinned(i)) continue;
        momentum_settings_mainmenu_push(
            app, FLIPPER_EXTERNAL_APPS[i].name, FLIPPER_EXTERNAL_APPS[i].name);
    }
}

/* Mirrors the parse in components/loader/loader_menu.c, so what this page shows
 * is what the menu will build. */
static void momentum_settings_mainmenu_add_line(MomentumSettingsApp* app, FuriString* line) {
    if(furi_string_start_with(line, "/")) {
        FuriString* label = furi_string_alloc();
        const Icon* icon;
        if(loader_menu_load_fap_meta(app->storage, line, label, &icon)) {
            loader_menu_free_fap_icon(icon);
        } else {
            furi_string_set(label, line);
        }
        momentum_settings_mainmenu_push(
            app, furi_string_get_cstr(label), furi_string_get_cstr(line));
        furi_string_free(label);
        return;
    }

    for(size_t i = 0; i < FLIPPER_APPS_COUNT; i++) {
        if(furi_string_equal(line, FLIPPER_APPS[i].name)) {
            momentum_settings_mainmenu_push(app, FLIPPER_APPS[i].name, FLIPPER_APPS[i].name);
            return;
        }
    }

    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; i++) {
        if(momentum_settings_mainmenu_is_pinned(i)) continue;
        if(furi_string_equal(line, FLIPPER_EXTERNAL_APPS[i].name)) {
            momentum_settings_mainmenu_push(
                app, FLIPPER_EXTERNAL_APPS[i].name, FLIPPER_EXTERNAL_APPS[i].name);
            return;
        }
    }
}

static void momentum_settings_mainmenu_clear(MomentumSettingsApp* app) {
    for(uint8_t i = 0; i < app->mainmenu_count; i++) {
        free(app->mainmenu_labels[i]);
        free(app->mainmenu_lines[i]);
    }
    app->mainmenu_count = 0;
}

static void momentum_settings_mainmenu_load(MomentumSettingsApp* app) {
    Stream* stream = file_stream_alloc(app->storage);
    FuriString* line = furi_string_alloc();
    unsigned long version;

    if(file_stream_open(stream, MAINMENU_APPS_PATH, FSAM_READ, FSOM_OPEN_EXISTING) &&
       stream_read_line(stream, line) &&
       sscanf(furi_string_get_cstr(line), MAINMENU_APPS_HEADER_FMT, &version) == 1 &&
       version == MAINMENU_APPS_VERSION) {
        while(stream_read_line(stream, line)) {
            furi_string_trim(line);
            if(furi_string_size(line)) momentum_settings_mainmenu_add_line(app, line);
        }
    } else {
        momentum_settings_mainmenu_load_defaults(app);
    }

    furi_string_free(line);
    file_stream_close(stream);
    stream_free(stream);
}

static void momentum_settings_mainmenu_save(MomentumSettingsApp* app) {
    Stream* stream = file_stream_alloc(app->storage);

    if(file_stream_open(stream, MAINMENU_APPS_PATH, FSAM_READ_WRITE, FSOM_CREATE_ALWAYS)) {
        stream_write_format(stream, MAINMENU_APPS_HEADER_FMT "\n", MAINMENU_APPS_VERSION);
        for(uint8_t i = 0; i < app->mainmenu_count; i++) {
            stream_write_format(stream, "%s\n", app->mainmenu_lines[i]);
        }
    } else {
        FURI_LOG_E(TAG, "Main menu layout could not be saved");
    }

    file_stream_close(stream);
    stream_free(stream);
}

static void momentum_settings_mainmenu_remove(MomentumSettingsApp* app, uint8_t index) {
    if(index >= app->mainmenu_count) return;

    free(app->mainmenu_labels[index]);
    free(app->mainmenu_lines[index]);

    const size_t tail = (app->mainmenu_count - index - 1U) * sizeof(char*);
    memmove(&app->mainmenu_labels[index], &app->mainmenu_labels[index + 1U], tail);
    memmove(&app->mainmenu_lines[index], &app->mainmenu_lines[index + 1U], tail);
    app->mainmenu_count--;
}

static void momentum_settings_mainmenu_swap(MomentumSettingsApp* app, uint8_t a, uint8_t b) {
    char* label = app->mainmenu_labels[a];
    char* line = app->mainmenu_lines[a];
    app->mainmenu_labels[a] = app->mainmenu_labels[b];
    app->mainmenu_lines[a] = app->mainmenu_lines[b];
    app->mainmenu_labels[b] = label;
    app->mainmenu_lines[b] = line;
}

/* Rewrites the rows that depend on the list after it changes, so add, remove
 * and move do not each have to rebuild the page. */
static void momentum_settings_mainmenu_refresh(MomentumSettingsApp* app) {
    VariableItem* item = variable_item_list_get(app->variable_item_list, MomentumMainmenuRowItem);
    if(!item) return;

    if(app->mainmenu_count) {
        if(app->mainmenu_index >= app->mainmenu_count) {
            app->mainmenu_index = app->mainmenu_count - 1U;
        }
        snprintf(
            app->mainmenu_item_label,
            sizeof(app->mainmenu_item_label),
            "Item %u/%u",
            (unsigned)(app->mainmenu_index + 1U),
            (unsigned)app->mainmenu_count);
        variable_item_set_current_value_text(item, app->mainmenu_labels[app->mainmenu_index]);
    } else {
        app->mainmenu_index = 0;
        snprintf(app->mainmenu_item_label, sizeof(app->mainmenu_item_label), "Item");
        variable_item_set_current_value_text(item, "None");
    }
    variable_item_set_item_label(item, app->mainmenu_item_label);
    variable_item_set_values_count(item, app->mainmenu_count);
    variable_item_set_current_value_index(item, app->mainmenu_index);

    VariableItem* move = variable_item_list_get(app->variable_item_list, MomentumMainmenuRowMove);
    if(move) {
        variable_item_set_locked(
            move, app->mainmenu_count < 2U, "Need at least\ntwo items\nto move");
    }
}

static void momentum_settings_mainmenu_item_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    app->mainmenu_index = variable_item_get_current_value_index(item);
    momentum_settings_mainmenu_refresh(app);
}

static const char* const momentum_mainmenu_move_text[] = {
    "Up",
    "Move",
    "Down",
};

static void momentum_settings_mainmenu_move_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    const uint8_t direction = variable_item_get_current_value_index(item);

    if(direction == 0U && app->mainmenu_index > 0U) {
        momentum_settings_mainmenu_swap(app, app->mainmenu_index, app->mainmenu_index - 1U);
        app->mainmenu_index--;
        app->mainmenu_dirty = true;
    } else if(direction == 2U && app->mainmenu_index + 1U < app->mainmenu_count) {
        momentum_settings_mainmenu_swap(app, app->mainmenu_index, app->mainmenu_index + 1U);
        app->mainmenu_index++;
        app->mainmenu_dirty = true;
    }

    /* Snaps back to the neutral value so the next turn in either direction is
     * another single step. */
    variable_item_set_current_value_index(item, 1U);
    variable_item_set_current_value_text(item, momentum_mainmenu_move_text[1]);
    momentum_settings_mainmenu_refresh(app);
}

static void momentum_settings_mainmenu_submenu_callback(void* context, uint32_t index);

/* Built-in apps are offered in one flat list, so the submenu index has to map
 * back through the same enumeration order the picker was built with. */
static const char* momentum_settings_mainmenu_builtin_name(uint32_t index) {
    if(index < FLIPPER_APPS_COUNT) return FLIPPER_APPS[index].name;

    uint32_t remaining = index - FLIPPER_APPS_COUNT;
    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; i++) {
        if(momentum_settings_mainmenu_is_pinned(i)) continue;
        if(remaining == 0U) return FLIPPER_EXTERNAL_APPS[i].name;
        remaining--;
    }

    return NULL;
}

static void momentum_settings_mainmenu_show_add_source(MomentumSettingsApp* app) {
    app->mainmenu_screen = MomentumMainmenuScreenAddSource;
    submenu_reset(app->mainmenu_submenu);
    submenu_set_header(app->mainmenu_submenu, "Add Menu Item");
    submenu_add_item(
        app->mainmenu_submenu, "Built-in App", 0, momentum_settings_mainmenu_submenu_callback, app);
    submenu_add_item(
        app->mainmenu_submenu,
        "App or Script",
        1,
        momentum_settings_mainmenu_submenu_callback,
        app);
    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumSettingsViewMainmenu);
}

static void momentum_settings_mainmenu_show_add_builtin(MomentumSettingsApp* app) {
    app->mainmenu_screen = MomentumMainmenuScreenAddBuiltin;
    submenu_reset(app->mainmenu_submenu);
    submenu_set_header(app->mainmenu_submenu, "Built-in App");

    uint32_t index = 0;
    for(size_t i = 0; i < FLIPPER_APPS_COUNT; i++) {
        submenu_add_item(
            app->mainmenu_submenu,
            FLIPPER_APPS[i].name,
            index++,
            momentum_settings_mainmenu_submenu_callback,
            app);
    }
    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; i++) {
        if(momentum_settings_mainmenu_is_pinned(i)) continue;
        submenu_add_item(
            app->mainmenu_submenu,
            FLIPPER_EXTERNAL_APPS[i].name,
            index++,
            momentum_settings_mainmenu_submenu_callback,
            app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumSettingsViewMainmenu);
}

/* A submenu rather than a DialogEx: DialogEx puts its buttons on Left and
 * Right, which on this board means holding the encoder while turning, with no
 * on-screen hint that this is what a confirmation needs. */
static void momentum_settings_mainmenu_show_reset(MomentumSettingsApp* app) {
    app->mainmenu_screen = MomentumMainmenuScreenResetConfirm;
    submenu_reset(app->mainmenu_submenu);
    submenu_set_header(app->mainmenu_submenu, "Reset menu items?");
    submenu_add_item(
        app->mainmenu_submenu, "Cancel", 0, momentum_settings_mainmenu_submenu_callback, app);
    submenu_add_item(
        app->mainmenu_submenu, "Reset", 1, momentum_settings_mainmenu_submenu_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumSettingsViewMainmenu);
}

static bool momentum_settings_mainmenu_browser_item(
    FuriString* path,
    void* context,
    uint8_t** icon_ptr,
    FuriString* item_name) {
    MomentumSettingsApp* app = context;
    return flipper_application_load_name_and_icon(path, app->storage, icon_ptr, item_name);
}

static void momentum_settings_mainmenu_add_file(MomentumSettingsApp* app) {
    DialogsFileBrowserOptions options = {
        .extension = ".fap|.js",
        .base_path = EXT_PATH("apps"),
        .skip_assets = true,
        .hide_dot_files = true,
        .icon = &I_unknown_10px,
        .hide_ext = true,
        .item_loader_callback = momentum_settings_mainmenu_browser_item,
        .item_loader_context = app,
    };

    FuriString* path = furi_string_alloc_set_str(options.base_path);
    if(dialog_file_browser_show(app->dialogs, path, path, &options)) {
        const uint8_t before = app->mainmenu_count;
        momentum_settings_mainmenu_add_line(app, path);
        if(app->mainmenu_count > before) {
            app->mainmenu_index = app->mainmenu_count - 1U;
            app->mainmenu_dirty = true;
        }
    }
    furi_string_free(path);

    momentum_settings_show_page(app, MomentumSettingsPageMainmenu, MomentumMainmenuRowAdd);
}

static void momentum_settings_mainmenu_submenu_callback(void* context, uint32_t index) {
    MomentumSettingsApp* app = context;
    uint8_t return_row = MomentumMainmenuRowAdd;

    switch(app->mainmenu_screen) {
    case MomentumMainmenuScreenAddSource:
        /* Both branches move to another screen of their own. */
        if(index == 0U) {
            momentum_settings_mainmenu_show_add_builtin(app);
        } else {
            momentum_settings_mainmenu_add_file(app);
        }
        return;

    case MomentumMainmenuScreenAddBuiltin: {
        const char* name = momentum_settings_mainmenu_builtin_name(index);
        if(name && momentum_settings_mainmenu_push(app, name, name)) {
            app->mainmenu_index = app->mainmenu_count - 1U;
            app->mainmenu_dirty = true;
        }
        break;
    }

    case MomentumMainmenuScreenResetConfirm:
        return_row = MomentumMainmenuRowReset;
        if(index == 1U) {
            storage_simply_remove(app->storage, MAINMENU_APPS_PATH);
            momentum_settings_mainmenu_clear(app);
            momentum_settings_mainmenu_load_defaults(app);
            app->mainmenu_index = 0;
            /* The file is gone and the list is back to the default order, so
             * there is nothing left for the exit path to write. */
            app->mainmenu_dirty = false;
        }
        break;
    }

    momentum_settings_show_page(app, MomentumSettingsPageMainmenu, return_row);
}

static uint32_t momentum_settings_back_to_list(void* context) {
    UNUSED(context);
    return MomentumSettingsViewList;
}

static void momentum_settings_lock_if_storage_unavailable(
    const MomentumSettingsApp* app,
    VariableItem* item) {
    if(!app->sd_ready) variable_item_set_locked(item, true, "SD card required");
}

static void momentum_settings_add_bool_item(
    MomentumSettingsApp* app,
    const char* label,
    bool value,
    VariableItemChangeCallback callback) {
    VariableItem* item = variable_item_list_add(
        app->variable_item_list,
        label,
        COUNT_OF(momentum_unlock_anim_values),
        callback,
        app);
    uint8_t value_index = value ? 1U : 0U;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);
}

static void momentum_settings_show_page(
    MomentumSettingsApp* app,
    MomentumSettingsPage page,
    uint8_t selected_item) {
    furi_assert(app);

    variable_item_list_reset(app->variable_item_list);
    app->asset_pack_item = NULL;
    app->current_page = page;

    VariableItem* item;
    uint8_t value_index;

    if(page == MomentumSettingsPageRoot) {
        variable_item_list_set_header(app->variable_item_list, "Momentum Settings");
        variable_item_list_add(app->variable_item_list, "Interface", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Protocols", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Misc", 1, NULL, app);
    } else if(page == MomentumSettingsPageInterface) {
        variable_item_list_set_header(app->variable_item_list, "Interface");
        variable_item_list_add(app->variable_item_list, "Graphics", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Mainmenu", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Lockscreen", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Statusbar", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "File Browser", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "General", 1, NULL, app);
    } else if(page == MomentumSettingsPageMainmenu) {
        variable_item_list_set_header(app->variable_item_list, "Mainmenu");

        item = variable_item_list_add(
            app->variable_item_list,
            "Menu Style",
            COUNT_OF(momentum_menu_style_text),
            momentum_settings_menu_style_changed,
            app);
        value_index = (uint8_t)app->settings.menu_style;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_menu_style_text[value_index]);

        variable_item_list_add(app->variable_item_list, "Reset Menu", 1, NULL, app);

        variable_item_list_add(
            app->variable_item_list,
            "Item",
            app->mainmenu_count,
            momentum_settings_mainmenu_item_changed,
            app);

        variable_item_list_add(app->variable_item_list, "Add Item", 1, NULL, app);

        /* Placed on the Mainmenu page because that is what it changes: the
         * Dual Boot entry reboots into another firmware, which is not
         * something to leave one OK press away on a shared device. */
        item = variable_item_list_add(
            app->variable_item_list,
            "Hide Dual Boot",
            COUNT_OF(momentum_unlock_anim_text),
            momentum_settings_hide_dualboot_changed,
            app);
        value_index = app->settings.hide_dualboot ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Move Item",
            COUNT_OF(momentum_mainmenu_move_text),
            momentum_settings_mainmenu_move_changed,
            app);
        variable_item_set_current_value_index(item, 1U);
        variable_item_set_current_value_text(item, momentum_mainmenu_move_text[1]);

        variable_item_list_add(app->variable_item_list, "Remove Item", 1, NULL, app);

        /* Fills in the Item row's label and value and the Move lock from the
         * list, so those three places are written in one function only. */
        momentum_settings_mainmenu_refresh(app);
    } else if(page == MomentumSettingsPageGraphics) {
        variable_item_list_set_header(app->variable_item_list, "Graphics");

        app->asset_pack_item = variable_item_list_add(
            app->variable_item_list, "Asset Pack", 1, NULL, app);
        momentum_settings_update_asset_pack_item(app);
        momentum_settings_lock_if_storage_unavailable(app, app->asset_pack_item);

        item = variable_item_list_add(
            app->variable_item_list,
            "Anim Speed",
            COUNT_OF(momentum_anim_speed_values),
            momentum_settings_anim_speed_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            app->settings.anim_speed,
            momentum_anim_speed_values,
            COUNT_OF(momentum_anim_speed_values));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_anim_speed_text[value_index]);
        momentum_settings_lock_if_storage_unavailable(app, item);

        item = variable_item_list_add(
            app->variable_item_list,
            "Cycle Anims",
            COUNT_OF(momentum_cycle_anim_values),
            momentum_settings_cycle_anims_changed,
            app);
        value_index = (uint8_t)value_index_int32(
            app->settings.cycle_anims,
            momentum_cycle_anim_values,
            COUNT_OF(momentum_cycle_anim_values));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_cycle_anim_text[value_index]);
        momentum_settings_lock_if_storage_unavailable(app, item);

        item = variable_item_list_add(
            app->variable_item_list,
            "Unlock Anims",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_unlock_anims_changed,
            app);
        value_index = app->settings.unlock_anims ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);
        momentum_settings_lock_if_storage_unavailable(app, item);
    } else if(page == MomentumSettingsPageLockscreen) {
        variable_item_list_set_header(app->variable_item_list, "Lockscreen");

        momentum_settings_add_bool_item(
            app,
            "Lock On Boot",
            app->settings.lock_on_boot,
            momentum_settings_lock_on_boot_changed);
        momentum_settings_add_bool_item(
            app,
            "Locked USB RPC",
            app->settings.allow_locked_rpc_usb,
            momentum_settings_locked_rpc_usb_changed);
        momentum_settings_add_bool_item(
            app,
            "Locked BLE RPC",
            app->settings.allow_locked_rpc_ble,
            momentum_settings_locked_rpc_ble_changed);
        momentum_settings_add_bool_item(
            app,
            "Allow Poweroff",
            app->settings.lockscreen_poweroff,
            momentum_settings_lockscreen_poweroff_changed);
        momentum_settings_add_bool_item(
            app,
            "Show Time",
            app->settings.lockscreen_time,
            momentum_settings_lockscreen_time_changed);
        momentum_settings_add_bool_item(
            app,
            "Show Seconds",
            app->settings.lockscreen_seconds,
            momentum_settings_lockscreen_seconds_changed);
        momentum_settings_add_bool_item(
            app,
            "Show Date",
            app->settings.lockscreen_date,
            momentum_settings_lockscreen_date_changed);
        momentum_settings_add_bool_item(
            app,
            "Show Statusbar",
            app->settings.lockscreen_statusbar,
            momentum_settings_lockscreen_statusbar_changed);
        momentum_settings_add_bool_item(
            app,
            "Unlock Prompt",
            app->settings.lockscreen_prompt,
            momentum_settings_lockscreen_prompt_changed);
        momentum_settings_add_bool_item(
            app,
            "Transparent (see animation)",
            app->settings.lockscreen_transparent,
            momentum_settings_lockscreen_transparent_changed);
        momentum_settings_add_bool_item(
            app,
            "Skip Sliding Animation",
            app->settings.lockscreen_skip_animation,
            momentum_settings_lockscreen_skip_animation_changed);
    } else if(page == MomentumSettingsPageStatusbar) {
        variable_item_list_set_header(app->variable_item_list, "Statusbar");

        item = variable_item_list_add(
            app->variable_item_list,
            "Battery Icon",
            COUNT_OF(momentum_battery_icon_values),
            momentum_settings_battery_icon_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            app->desktop_settings.displayBatteryPercentage,
            momentum_battery_icon_values,
            COUNT_OF(momentum_battery_icon_values));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_battery_icon_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Show Clock",
            COUNT_OF(momentum_clock_values),
            momentum_settings_clock_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            app->desktop_settings.display_clock,
            momentum_clock_values,
            COUNT_OF(momentum_clock_values));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Status Icons",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_status_icons_changed,
            app);
        value_index = app->settings.status_icons ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Bar Borders",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_bar_borders_changed,
            app);
        value_index = app->settings.bar_borders ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Bar Background",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_bar_background_changed,
            app);
        value_index = app->settings.bar_background ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);
    } else if(page == MomentumSettingsPageFileBrowser) {
        variable_item_list_set_header(app->variable_item_list, "File Browser");

        item = variable_item_list_add(
            app->variable_item_list,
            "Folders Above Files",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_sort_dirs_first_changed,
            app);
        value_index = app->settings.sort_dirs_first ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Show Hidden Files",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_show_hidden_files_changed,
            app);
        value_index = app->settings.show_hidden_files ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Show Internal Tab",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_show_internal_tab_changed,
            app);
        value_index = app->settings.show_internal_tab ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Show Path",
            COUNT_OF(momentum_browser_path_text),
            momentum_settings_browser_path_changed,
            app);
        value_index = (uint8_t)app->settings.browser_path_mode;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_browser_path_text[value_index]);

    } else if(page == MomentumSettingsPageGeneral) {
        variable_item_list_set_header(app->variable_item_list, "General");

        item = variable_item_list_add(
            app->variable_item_list,
            "Text Scroll",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_scroll_changed,
            app);
        value_index = app->settings.scroll_marquee ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, value_index ? "Marquee" : "Standard");

        item = variable_item_list_add(
            app->variable_item_list,
            "Midnight Format",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_midnight_changed,
            app);
        value_index = app->settings.midnight_format_00 ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, value_index ? "00:XX" : "12:XX");

        item = variable_item_list_add(
            app->variable_item_list,
            "Popup Overlay",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_popup_overlay_changed,
            app);
        value_index = app->settings.popup_overlay ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);
    } else if(page == MomentumSettingsPageProtocols) {
        variable_item_list_set_header(app->variable_item_list, "Protocols");

        item = variable_item_list_add(
            app->variable_item_list,
            "File Naming",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_file_naming_changed,
            app);
        value_index = app->settings.file_naming_prefix_after ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, value_index ? "After" : "Before");

        item = variable_item_list_add(
            app->variable_item_list,
            "Extended Range",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_subghz_extended_changed,
            app);
        value_index = app->subghz_extended_range ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);

        variable_item_list_add(app->variable_item_list, "Frequencies", 1, NULL, app);
    } else if(page == MomentumSettingsPageDolphin) {
        variable_item_list_set_header(app->variable_item_list, "Dolphin");

        char label[16];
        item = variable_item_list_add(app->variable_item_list, "Level", 1, NULL, app);
        snprintf(label, sizeof(label), "%u", (unsigned)dolphin_get_level(app->dolphin_xp));
        variable_item_set_current_value_text(item, label);

        item = variable_item_list_add(app->variable_item_list, "XP", 1, NULL, app);
        snprintf(label, sizeof(label), "%lu", (unsigned long)app->dolphin_xp);
        variable_item_set_current_value_text(item, label);

        item = variable_item_list_add(
            app->variable_item_list,
            "Mood",
            MOMENTUM_DOLPHIN_BUTTHURT_MAX + 1,
            momentum_settings_butthurt_changed,
            app);
        value_index = (uint8_t)CLAMP(app->dolphin_butthurt, MOMENTUM_DOLPHIN_BUTTHURT_MAX, 0);
        variable_item_set_current_value_index(item, value_index);
        snprintf(label, sizeof(label), "%u", (unsigned)value_index);
        variable_item_set_current_value_text(item, label);
    } else if(page == MomentumSettingsPageFrequencies) {
        variable_item_list_set_header(app->variable_item_list, "Frequencies");

        item = variable_item_list_add(
            app->variable_item_list,
            "Default Freqs",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_use_defaults_changed,
            app);
        value_index = app->subghz_use_defaults ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);

        variable_item_list_add(app->variable_item_list, "Static Freqs", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Hopper Freqs", 1, NULL, app);
    } else if(page == MomentumSettingsPageMisc) {
        variable_item_list_set_header(app->variable_item_list, "Misc");
        variable_item_list_add(app->variable_item_list, "Screen", 1, NULL, app);
        item = variable_item_list_add(app->variable_item_list, "Device Name", 1, NULL, app);
        variable_item_set_current_value_text(
            item, app->device_name[0] ? app->device_name : "Default");

        item = variable_item_list_add(
            app->variable_item_list,
            "Dolphin Sad Timer",
            COUNT_OF(momentum_butthurt_timer_values),
            momentum_settings_butthurt_timer_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            app->settings.butthurt_timer,
            momentum_butthurt_timer_values,
            COUNT_OF(momentum_butthurt_timer_values));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_butthurt_timer_text[value_index]);

        /* Kept last so the rows above keep the indices the enter handler maps. */
        variable_item_list_add(app->variable_item_list, "Dolphin", 1, NULL, app);

        /* The slideshow is embedded in the firmware now, so a missing card asset
         * is no longer a reason to lock this: the old "needs firstboot.bin"
         * check would block the feature it was written to guard. The SD card is
         * still required, because staging writes the slideshow to it. */
        item = variable_item_list_add(
            app->variable_item_list, "Show Momentum Intro", 1, NULL, app);
        if(!app->sd_ready) {
            variable_item_set_locked(item, true, "SD card required");
        }
    } else if(page == MomentumSettingsPageScreen) {
        variable_item_list_set_header(app->variable_item_list, "Screen");

        item = variable_item_list_add(
            app->variable_item_list,
            "Dark Mode",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_dark_mode_changed,
            app);
        value_index = app->settings.dark_mode ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Left Handed",
            COUNT_OF(momentum_unlock_anim_values),
            momentum_settings_left_handed_changed,
            app);
        value_index = furi_hal_rtc_is_flag_set(FuriHalRtcFlagHandOrient) ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, momentum_unlock_anim_text[value_index]);
    }

    variable_item_list_set_selected_item(app->variable_item_list, selected_item);
    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumSettingsViewList);
}

static bool momentum_settings_custom_event(void* context, uint32_t event) {
    MomentumSettingsApp* app = context;
    furi_assert(app);

    /* Bounded by the enum's sentinel rather than by whichever page happened to
     * be last, so adding a page cannot silently drop its navigation event. */
    if(event < MomentumSettingsPageInterface || event >= MomentumSettingsPageCount) {
        return false;
    }

    momentum_settings_show_page(app, (MomentumSettingsPage)event, 0);
    return true;
}

static bool momentum_settings_back_event(void* context) {
    MomentumSettingsApp* app = context;
    furi_assert(app);

    if(app->current_page == MomentumSettingsPageRoot) {
        return false;
    } else if(app->current_page == MomentumSettingsPageInterface) {
        momentum_settings_show_page(app, MomentumSettingsPageRoot, 0);
    } else if(app->current_page == MomentumSettingsPageGraphics) {
        momentum_settings_show_page(app, MomentumSettingsPageInterface, 0);
    } else if(app->current_page == MomentumSettingsPageMainmenu) {
        momentum_settings_show_page(app, MomentumSettingsPageInterface, 1);
    } else if(app->current_page == MomentumSettingsPageLockscreen) {
        momentum_settings_show_page(app, MomentumSettingsPageInterface, 2);
    } else if(app->current_page == MomentumSettingsPageStatusbar) {
        momentum_settings_show_page(app, MomentumSettingsPageInterface, 3);
    } else if(app->current_page == MomentumSettingsPageFileBrowser) {
        momentum_settings_show_page(app, MomentumSettingsPageInterface, 4);
    } else if(app->current_page == MomentumSettingsPageGeneral) {
        momentum_settings_show_page(app, MomentumSettingsPageInterface, 5);
    } else if(app->current_page == MomentumSettingsPageProtocols) {
        momentum_settings_show_page(app, MomentumSettingsPageRoot, 1);
    } else if(app->current_page == MomentumSettingsPageMisc) {
        momentum_settings_show_page(app, MomentumSettingsPageRoot, 2);
    } else if(app->current_page == MomentumSettingsPageFrequencies) {
        momentum_settings_show_page(app, MomentumSettingsPageProtocols, 2);
    } else if(app->current_page == MomentumSettingsPageDolphin) {
        momentum_settings_show_page(app, MomentumSettingsPageMisc, 3);
    } else if(app->current_page == MomentumSettingsPageScreen) {
        momentum_settings_show_page(app, MomentumSettingsPageMisc, 0);
    }

    return true;
}

/* Reads the name the namechanger service applies at boot. Seeding from the
 * effective name instead would show the eFuse-derived default as though it
 * were a custom one. */
static void momentum_settings_load_device_name(MomentumSettingsApp* app) {
    FlipperFormat* file = flipper_format_file_alloc(app->storage);
    FuriString* str = furi_string_alloc();

    do {
        uint32_t version;
        if(!flipper_format_file_open_existing(file, NAMECHANGER_PATH)) break;
        if(!flipper_format_read_header(file, str, &version)) break;
        if(furi_string_cmp_str(str, NAMECHANGER_HEADER)) break;
        if(version != NAMECHANGER_VERSION) break;
        if(!flipper_format_read_string(file, "Name", str)) break;
        strncpy(app->device_name, furi_string_get_cstr(str), sizeof(app->device_name) - 1);
    } while(false);

    furi_string_free(str);
    flipper_format_free(file);
}

/* Frequency lists live in the same file subghz_setting_load() reads, so edits
 * here are picked up by SubGHz and SubGHz Remote without a reboot of anything
 * but the app that reads them. */
static void momentum_settings_load_freqs(MomentumSettingsApp* app) {
    app->subghz_use_defaults = true;
    app->subghz_static_count = 0;
    app->subghz_hopper_count = 0;

    FlipperFormat* file = flipper_format_file_alloc(app->storage);
    FuriString* str = furi_string_alloc();

    do {
        uint32_t version;
        if(!flipper_format_file_open_existing(file, MOMENTUM_SUBGHZ_USER_SETTINGS)) break;
        if(!flipper_format_read_header(file, str, &version)) break;
        if(furi_string_cmp_str(str, MOMENTUM_SUBGHZ_SETTING_TYPE) ||
           version != MOMENTUM_SUBGHZ_SETTING_VERSION)
            break;

        bool use_defaults = true;
        flipper_format_read_bool(file, "Add_standard_frequencies", &use_defaults, 1);
        app->subghz_use_defaults = use_defaults;

        uint32_t value;
        if(flipper_format_rewind(file)) {
            while(app->subghz_static_count < MOMENTUM_MAX_USER_FREQS &&
                  flipper_format_read_uint32(file, "Frequency", &value, 1)) {
                app->subghz_static_freqs[app->subghz_static_count++] = value;
            }
        }

        if(flipper_format_rewind(file)) {
            while(app->subghz_hopper_count < MOMENTUM_MAX_USER_FREQS &&
                  flipper_format_read_uint32(file, "Hopper_frequency", &value, 1)) {
                app->subghz_hopper_freqs[app->subghz_hopper_count++] = value;
            }
        }
    } while(false);

    furi_string_free(str);
    flipper_format_free(file);
}

static void momentum_settings_save_freqs(MomentumSettingsApp* app) {
    FlipperFormat* file = flipper_format_file_alloc(app->storage);

    do {
        if(!flipper_format_file_open_always(file, MOMENTUM_SUBGHZ_USER_SETTINGS)) break;
        if(!flipper_format_write_header_cstr(
               file, MOMENTUM_SUBGHZ_SETTING_TYPE, MOMENTUM_SUBGHZ_SETTING_VERSION))
            break;

        /* Keys are deleted before rewriting: flipper_format appends, so without
         * this every save would leave the previous entries in place. */
        while(flipper_format_delete_key(file, "Add_standard_frequencies"))
            ;
        flipper_format_write_bool(
            file, "Add_standard_frequencies", &app->subghz_use_defaults, 1);

        if(!flipper_format_rewind(file)) break;
        while(flipper_format_delete_key(file, "Frequency"))
            ;
        for(uint8_t i = 0; i < app->subghz_static_count; i++) {
            flipper_format_write_uint32(file, "Frequency", &app->subghz_static_freqs[i], 1);
        }

        if(!flipper_format_rewind(file)) break;
        while(flipper_format_delete_key(file, "Hopper_frequency"))
            ;
        for(uint8_t i = 0; i < app->subghz_hopper_count; i++) {
            flipper_format_write_uint32(
                file, "Hopper_frequency", &app->subghz_hopper_freqs[i], 1);
        }
    } while(false);

    flipper_format_free(file);
}

static uint32_t* momentum_settings_freq_list(MomentumSettingsApp* app, uint8_t** count) {
    if(app->subghz_editing_hopper) {
        *count = &app->subghz_hopper_count;
        return app->subghz_hopper_freqs;
    }
    *count = &app->subghz_static_count;
    return app->subghz_static_freqs;
}

static void momentum_settings_freq_submenu_callback(void* context, uint32_t index);

static void momentum_settings_show_freq_list(MomentumSettingsApp* app) {
    uint8_t* count;
    uint32_t* freqs = momentum_settings_freq_list(app, &count);

    submenu_reset(app->freq_submenu);
    submenu_set_header(
        app->freq_submenu, app->subghz_editing_hopper ? "Hopper Freqs" : "Static Freqs");

    static char labels[MOMENTUM_MAX_USER_FREQS][24];
    for(uint8_t i = 0; i < *count; i++) {
        snprintf(
            labels[i],
            sizeof(labels[i]),
            "%lu.%02lu MHz",
            (unsigned long)(freqs[i] / 1000000UL),
            (unsigned long)((freqs[i] % 1000000UL) / 10000UL));
        submenu_add_item(
            app->freq_submenu, labels[i], i, momentum_settings_freq_submenu_callback, app);
    }

    if(*count < MOMENTUM_MAX_USER_FREQS) {
        submenu_add_item(
            app->freq_submenu,
            "Add frequency...",
            MOMENTUM_MAX_USER_FREQS,
            momentum_settings_freq_submenu_callback,
            app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumSettingsViewFreqList);
}

static void momentum_settings_number_done(void* context, int32_t number) {
    MomentumSettingsApp* app = context;

    if(app->number_input_for_xp) {
        app->dolphin_xp = (uint32_t)number;
        app->dolphin_dirty = true;
        momentum_settings_show_page(app, MomentumSettingsPageDolphin, 1);
        return;
    }

    uint8_t* count;
    uint32_t* freqs = momentum_settings_freq_list(app, &count);

    const uint32_t value = (uint32_t)number * 1000UL; // entered in kHz
    if(*count < MOMENTUM_MAX_USER_FREQS && furi_hal_subghz_is_frequency_valid(value)) {
        freqs[(*count)++] = value;
        app->subghz_freqs_dirty = true;
    }
    momentum_settings_show_freq_list(app);
}

static void momentum_settings_freq_submenu_callback(void* context, uint32_t index) {
    MomentumSettingsApp* app = context;
    uint8_t* count;
    uint32_t* freqs = momentum_settings_freq_list(app, &count);

    if(index == MOMENTUM_MAX_USER_FREQS) {
        app->number_input_for_xp = false;
        number_input_set_header_text(app->number_input, "Frequency in kHz");
        number_input_set_result_callback(
            app->number_input, momentum_settings_number_done, app, 433920, 281000, 962000);
        view_dispatcher_switch_to_view(app->view_dispatcher, MomentumSettingsViewNumberInput);
        return;
    }

    /* Selecting an entry removes it; there is nothing else to do to one. */
    if(index < *count) {
        memmove(&freqs[index], &freqs[index + 1], (*count - index - 1) * sizeof(uint32_t));
        (*count)--;
        app->subghz_freqs_dirty = true;
    }
    momentum_settings_show_freq_list(app);
}

static void momentum_settings_butthurt_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    char label[8];
    snprintf(label, sizeof(label), "%u", (unsigned)index);
    variable_item_set_current_value_text(item, label);
    app->dolphin_butthurt = (int32_t)index;
    app->dolphin_dirty = true;
}

static void momentum_settings_use_defaults_changed(VariableItem* item) {
    MomentumSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, momentum_unlock_anim_text[index]);
    app->subghz_use_defaults = momentum_unlock_anim_values[index];
    app->subghz_freqs_dirty = true;
}

static MomentumSettingsApp* momentum_settings_app_alloc(void) {
    MomentumSettingsApp* app = malloc(sizeof(MomentumSettingsApp));
    memset(app, 0, sizeof(MomentumSettingsApp));
    app->settings = momentum_settings;
    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->desktop = furi_record_open(RECORD_DESKTOP);
    app->power = furi_record_open(RECORD_POWER);
    app->dolphin = furi_record_open(RECORD_DOLPHIN);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    DolphinStats stats = dolphin_stats(app->dolphin);
    app->dolphin_xp = stats.icounter;
    app->dolphin_butthurt = (int32_t)stats.butthurt;
    desktop_api_get_settings(app->desktop, &app->desktop_settings);
    app->sd_ready = storage_sd_status(app->storage) == FSE_OK;

    momentum_settings_load_device_name(app);
    app->subghz_extended_range = subghz_extended_range_load();
    momentum_settings_load_freqs(app);
    momentum_settings_mainmenu_load(app);

    momentum_settings_scan_asset_packs(app);

    app->view_dispatcher = view_dispatcher_alloc();
    app->variable_item_list = variable_item_list_alloc();
    app->asset_pack_submenu = submenu_alloc();
    app->text_input = text_input_alloc();
    app->freq_submenu = submenu_alloc();
    app->mainmenu_submenu = submenu_alloc();
    app->number_input = number_input_alloc();

    View* list_view = variable_item_list_get_view(app->variable_item_list);
    variable_item_list_set_enter_callback(
        app->variable_item_list, momentum_settings_list_enter, app);
    view_dispatcher_add_view(
        app->view_dispatcher, MomentumSettingsViewList, list_view);

    View* asset_pack_view = submenu_get_view(app->asset_pack_submenu);
    view_set_previous_callback(asset_pack_view, momentum_settings_back_to_list);
    submenu_set_header(app->asset_pack_submenu, "Asset Pack");
    submenu_add_item(
        app->asset_pack_submenu, "Default", 0, momentum_settings_asset_pack_selected, app);
    for(uint8_t i = 0; i < app->asset_pack_count; i++) {
        submenu_add_item(
            app->asset_pack_submenu,
            app->asset_pack_names[i],
            i + 1U,
            momentum_settings_asset_pack_selected,
            app);
    }
    submenu_set_selected_item(
        app->asset_pack_submenu, momentum_settings_asset_pack_index(app));
    view_dispatcher_add_view(
        app->view_dispatcher, MomentumSettingsViewAssetPacks, asset_pack_view);

    View* text_input_view = text_input_get_view(app->text_input);
    view_set_previous_callback(text_input_view, momentum_settings_back_to_list);
    view_dispatcher_add_view(
        app->view_dispatcher, MomentumSettingsViewTextInput, text_input_view);

    View* freq_view = submenu_get_view(app->freq_submenu);
    view_set_previous_callback(freq_view, momentum_settings_back_to_list);
    view_dispatcher_add_view(app->view_dispatcher, MomentumSettingsViewFreqList, freq_view);

    View* mainmenu_view = submenu_get_view(app->mainmenu_submenu);
    view_set_previous_callback(mainmenu_view, momentum_settings_back_to_list);
    view_dispatcher_add_view(app->view_dispatcher, MomentumSettingsViewMainmenu, mainmenu_view);

    View* number_view = number_input_get_view(app->number_input);
    view_dispatcher_add_view(
        app->view_dispatcher, MomentumSettingsViewNumberInput, number_view);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, momentum_settings_custom_event);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, momentum_settings_back_event);
    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    momentum_settings_show_page(app, MomentumSettingsPageRoot, 0);
    return app;
}

static void momentum_settings_app_free(MomentumSettingsApp* app) {
    furi_assert(app);

    view_dispatcher_remove_view(app->view_dispatcher, MomentumSettingsViewNumberInput);
    view_dispatcher_remove_view(app->view_dispatcher, MomentumSettingsViewMainmenu);
    view_dispatcher_remove_view(app->view_dispatcher, MomentumSettingsViewFreqList);
    view_dispatcher_remove_view(app->view_dispatcher, MomentumSettingsViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, MomentumSettingsViewAssetPacks);
    view_dispatcher_remove_view(app->view_dispatcher, MomentumSettingsViewList);
    number_input_free(app->number_input);
    submenu_free(app->mainmenu_submenu);
    submenu_free(app->freq_submenu);
    text_input_free(app->text_input);
    submenu_free(app->asset_pack_submenu);
    variable_item_list_free(app->variable_item_list);
    view_dispatcher_free(app->view_dispatcher);

    for(uint8_t i = 0; i < app->asset_pack_count; i++) {
        free(app->asset_pack_names[i]);
    }
    free(app->asset_pack_names);
    momentum_settings_mainmenu_clear(app);

    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_DOLPHIN);
    furi_record_close(RECORD_POWER);
    furi_record_close(RECORD_DESKTOP);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t momentum_app(void* p) {
    UNUSED(p);
    MomentumSettingsApp* app = momentum_settings_app_alloc();
    view_dispatcher_run(app->view_dispatcher);

    if(app->dirty) {
        const bool pack_changed =
            strcmp(momentum_settings.asset_pack, app->settings.asset_pack) != 0;

        momentum_settings = app->settings;
        name_generator_set_prefix_after(momentum_settings.file_naming_prefix_after);
        if(!momentum_settings_save()) {
            FURI_LOG_E(TAG, "Settings are active for this session but were not saved");
        }

        if(pack_changed) {
            // Icons and fonts are held in RAM, so the old pack has to go before
            // the new one loads.
            asset_packs_free();
            asset_packs_init();
        }
    }

    if(app->name_dirty) {
        if(app->device_name[0] == '\0') {
            storage_simply_remove(app->storage, NAMECHANGER_PATH);
        } else {
            FlipperFormat* file = flipper_format_file_alloc(app->storage);
            do {
                if(!flipper_format_file_open_always(file, NAMECHANGER_PATH)) break;
                if(!flipper_format_write_header_cstr(
                       file, NAMECHANGER_HEADER, NAMECHANGER_VERSION))
                    break;
                if(!flipper_format_write_string_cstr(file, "Name", app->device_name)) break;
            } while(0);
            flipper_format_free(file);
        }
        // Apply without a reboot. furi_hal copies the string into its own
        // buffer, and passing NULL restores the eFuse-derived name.
        furi_hal_version_set_name(app->device_name[0] ? app->device_name : NULL);
    }

    if(app->subghz_freqs_dirty) {
        momentum_settings_save_freqs(app);
    }

    if(app->mainmenu_dirty) {
        momentum_settings_mainmenu_save(app);
    }

    if(app->dolphin_dirty) {
        /* Written straight into the dolphin's state: there is no public setter
         * for XP or mood, and this is what upstream does too. */
        app->dolphin->state->data.icounter = app->dolphin_xp;
        app->dolphin->state->data.butthurt = app->dolphin_butthurt;
        dolphin_state_save(app->dolphin->state);
    }

    if(app->desktop_dirty) {
        desktop_settings_save(&app->desktop_settings);
        desktop_api_set_settings(app->desktop, &app->desktop_settings);
        power_trigger_ui_update(app->power);
    }

    /* Everything above has been written, so it is safe to go down here. This
     * does not return. */
    if(app->reboot_for_intro) {
        power_reboot(app->power, PowerBootModeNormal);
    }

    momentum_settings_app_free(app);
    return 0;
}
