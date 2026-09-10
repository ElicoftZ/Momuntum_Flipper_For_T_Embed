#include "../hotspot_arcade_i.h"

#include <string.h>

#define HOTSPOT_ARCADE_SETTINGS_EVENT_BASE 0x3000U

typedef enum {
    HotspotArcadeSettingsItemLanguage = 1U,
    HotspotArcadeSettingsItemSound,
    HotspotArcadeSettingsItemVibro,
} HotspotArcadeSettingsItem;

static const char* hotspot_arcade_scene_settings_language_name(const char* lang) {
    if(!strcmp(lang, "de")) return "Deutsch";
    if(!strcmp(lang, "pt-br")) return "Portugues BR";
    return "English";
}

static void hotspot_arcade_scene_settings_submenu_callback(void* context, uint32_t index) {
    HotspotArcadeApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void hotspot_arcade_scene_settings_build(
    HotspotArcadeApp* app,
    HotspotArcadeSettingsItem selected) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Settings");

    furi_string_printf(
        app->display_text,
        "Language: %s",
        hotspot_arcade_scene_settings_language_name(app->settings.lang));
    submenu_add_item(
        app->submenu,
        furi_string_get_cstr(app->display_text),
        HOTSPOT_ARCADE_SETTINGS_EVENT_BASE + HotspotArcadeSettingsItemLanguage,
        hotspot_arcade_scene_settings_submenu_callback,
        app);

    submenu_add_item(
        app->submenu,
        app->settings.sound ? "Sound: On" : "Sound: Off",
        HOTSPOT_ARCADE_SETTINGS_EVENT_BASE + HotspotArcadeSettingsItemSound,
        hotspot_arcade_scene_settings_submenu_callback,
        app);
    submenu_add_item(
        app->submenu,
        app->settings.vibro ? "Vibro: On" : "Vibro: Off",
        HOTSPOT_ARCADE_SETTINGS_EVENT_BASE + HotspotArcadeSettingsItemVibro,
        hotspot_arcade_scene_settings_submenu_callback,
        app);
    submenu_set_selected_item(
        app->submenu, HOTSPOT_ARCADE_SETTINGS_EVENT_BASE + (uint32_t)selected);
}

void hotspot_arcade_scene_settings_on_enter(void* context) {
    HotspotArcadeApp* app = context;
    uint32_t selected =
        scene_manager_get_scene_state(app->scene_manager, HotspotArcadeSceneSettings);
    if(selected < HotspotArcadeSettingsItemLanguage ||
       selected > HotspotArcadeSettingsItemVibro) {
        selected = HotspotArcadeSettingsItemLanguage;
    }
    hotspot_arcade_scene_settings_build(app, (HotspotArcadeSettingsItem)selected);
    view_dispatcher_switch_to_view(app->view_dispatcher, HotspotArcadeViewSubmenu);
}

bool hotspot_arcade_scene_settings_on_event(void* context, SceneManagerEvent event) {
    HotspotArcadeApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event <= HOTSPOT_ARCADE_SETTINGS_EVENT_BASE ||
       event.event > HOTSPOT_ARCADE_SETTINGS_EVENT_BASE + HotspotArcadeSettingsItemVibro) {
        return event.event == HotspotArcadeEventSnapshotChanged;
    }

    const HotspotArcadeSettingsItem selected =
        (HotspotArcadeSettingsItem)(event.event - HOTSPOT_ARCADE_SETTINGS_EVENT_BASE);
    if(selected == HotspotArcadeSettingsItemLanguage) {
        if(!strcmp(app->settings.lang, "")) {
            strcpy(app->settings.lang, "de");
        } else if(!strcmp(app->settings.lang, "de")) {
            strcpy(app->settings.lang, "pt-br");
        } else {
            app->settings.lang[0] = '\0';
        }
    } else if(selected == HotspotArcadeSettingsItemSound) {
        app->settings.sound = !app->settings.sound;
    } else {
        app->settings.vibro = !app->settings.vibro;
    }

    scene_manager_set_scene_state(app->scene_manager, HotspotArcadeSceneSettings, selected);
    if(!hotspot_arcade_settings_save(app->storage, &app->settings)) {
        hotspot_arcade_show_message(
            app,
            "Could not save settings",
            "Check that the SD card is mounted and writable.");
        return true;
    }
    hotspot_arcade_feedback(app);
    hotspot_arcade_scene_settings_build(app, selected);
    return true;
}

void hotspot_arcade_scene_settings_on_exit(void* context) {
    HotspotArcadeApp* app = context;
    submenu_reset(app->submenu);
}
