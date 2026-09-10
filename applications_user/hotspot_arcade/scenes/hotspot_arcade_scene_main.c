#include "../hotspot_arcade_i.h"

typedef enum {
    HotspotArcadeMainItemSession = 1U,
    HotspotArcadeMainItemDashboard,
    HotspotArcadeMainItemGames,
    HotspotArcadeMainItemLeaderboard,
    HotspotArcadeMainItemConsole,
    HotspotArcadeMainItemSsid,
    HotspotArcadeMainItemSettings,
} HotspotArcadeMainItem;

static void hotspot_arcade_scene_main_submenu_callback(void* context, uint32_t index) {
    HotspotArcadeApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void hotspot_arcade_scene_main_update_session_label(HotspotArcadeApp* app) {
    submenu_change_item_label(
        app->submenu,
        HotspotArcadeMainItemSession,
        app->snapshot.running ? "Stop Session [LIVE]" : "Start Session");
}

static bool hotspot_arcade_scene_main_prepare_storage(HotspotArcadeApp* app) {
    /* storage_sd_status(), not storage_dir_exists(STORAGE_EXT_PATH_PREFIX): the
     * latter stats the bare mount root "/ext", which this port's path mapping
     * does not answer for the root itself, so it reported "SD card required"
     * on a perfectly mounted card. storage_sd_status() is what the rest of the
     * port uses for "is there a card" (see components/storage/asset_installer.c)
     * and is proven on hardware for both present and absent cards. */
    if(storage_sd_status(app->storage) != FSE_OK) {
        hotspot_arcade_show_message(
            app,
            "SD card required",
            "Insert and mount an SD card, then try Start Session again.");
        return false;
    }

    if(!storage_file_exists(app->storage, HOTSPOT_ARCADE_WEB_STORAGE_PATH)) {
        hotspot_arcade_show_message(
            app,
            "Web client missing",
            "Copy index.html.gz to:\n/ext/apps_assets/hotspot_arcade/web/");
        return false;
    }

    if(!storage_dir_exists(app->storage, HOTSPOT_ARCADE_PACKS_STORAGE_DIR)) {
        hotspot_arcade_show_message(
            app,
            "Game packs missing",
            "Copy the packs folder to:\n/ext/apps_assets/hotspot_arcade/packs/");
        return false;
    }

    if(!hotspot_arcade_settings_save(app->storage, &app->settings)) {
        hotspot_arcade_show_message(
            app,
            "Storage error",
            "Could not create /ext/apps_data/hotspot_arcade/config.txt");
        return false;
    }

    const char* user_packs_dir = HOTSPOT_ARCADE_CONFIG_DIR "/packs";
    if(!storage_dir_exists(app->storage, user_packs_dir) &&
       !storage_simply_mkdir(app->storage, user_packs_dir)) {
        hotspot_arcade_show_message(
            app,
            "Storage error",
            "Could not create /ext/apps_data/hotspot_arcade/packs/");
        return false;
    }
    return true;
}

static void hotspot_arcade_scene_main_start(HotspotArcadeApp* app) {
    if(!hotspot_arcade_scene_main_prepare_storage(app)) return;

    const HotspotArcadeServiceConfig config = {
        .ssid = app->settings.ssid,
        .web_gzip_path = HOTSPOT_ARCADE_WEB_SERVICE_PATH,
        .bundled_packs_dir = HOTSPOT_ARCADE_BUNDLED_PACKS_SERVICE_DIR,
        .user_packs_dir = HOTSPOT_ARCADE_USER_PACKS_SERVICE_DIR,
        .lang = app->settings.lang,
    };
    const HotspotArcadeServiceResult result = hotspot_arcade_service_start(&config);
    if(result != HotspotArcadeServiceResultOk &&
       result != HotspotArcadeServiceResultAlreadyRunning) {
        const char* reason = hotspot_arcade_service_result_to_string(result);
        furi_string_printf(
            app->display_text,
            "The ESP32-S3 service could not start.\n\nReason: %s",
            reason ? reason : "unknown error");
        hotspot_arcade_show_message(
            app, "Session start failed", furi_string_get_cstr(app->display_text));
        return;
    }

    hotspot_arcade_refresh_snapshot(app);
    hotspot_arcade_feedback(app);
    scene_manager_next_scene(app->scene_manager, HotspotArcadeSceneDashboard);
}

void hotspot_arcade_scene_main_on_enter(void* context) {
    HotspotArcadeApp* app = context;
    submenu_set_header(app->submenu, "Hotspot Arcade");
    submenu_add_item(
        app->submenu,
        app->snapshot.running ? "Stop Session [LIVE]" : "Start Session",
        HotspotArcadeMainItemSession,
        hotspot_arcade_scene_main_submenu_callback,
        app);
    submenu_add_item(
        app->submenu,
        "Dashboard",
        HotspotArcadeMainItemDashboard,
        hotspot_arcade_scene_main_submenu_callback,
        app);
    submenu_add_item(
        app->submenu,
        "Games (40)",
        HotspotArcadeMainItemGames,
        hotspot_arcade_scene_main_submenu_callback,
        app);
    submenu_add_item(
        app->submenu,
        "Leaderboard",
        HotspotArcadeMainItemLeaderboard,
        hotspot_arcade_scene_main_submenu_callback,
        app);
    submenu_add_item(
        app->submenu,
        "Console",
        HotspotArcadeMainItemConsole,
        hotspot_arcade_scene_main_submenu_callback,
        app);
    submenu_add_item(
        app->submenu,
        "Set SSID",
        HotspotArcadeMainItemSsid,
        hotspot_arcade_scene_main_submenu_callback,
        app);
    submenu_add_item(
        app->submenu,
        "Settings",
        HotspotArcadeMainItemSettings,
        hotspot_arcade_scene_main_submenu_callback,
        app);
    submenu_set_selected_item(
        app->submenu,
        scene_manager_get_scene_state(app->scene_manager, HotspotArcadeSceneMain));
    view_dispatcher_switch_to_view(app->view_dispatcher, HotspotArcadeViewSubmenu);
}

bool hotspot_arcade_scene_main_on_event(void* context, SceneManagerEvent event) {
    HotspotArcadeApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == HotspotArcadeEventSnapshotChanged) {
        hotspot_arcade_scene_main_update_session_label(app);
        return true;
    }

    scene_manager_set_scene_state(app->scene_manager, HotspotArcadeSceneMain, event.event);
    switch(event.event) {
    case HotspotArcadeMainItemSession:
        if(app->snapshot.running || hotspot_arcade_service_is_running()) {
            hotspot_arcade_service_stop();
            hotspot_arcade_refresh_snapshot(app);
            hotspot_arcade_feedback(app);
            hotspot_arcade_scene_main_update_session_label(app);
        } else {
            hotspot_arcade_scene_main_start(app);
        }
        return true;
    case HotspotArcadeMainItemDashboard:
        scene_manager_next_scene(app->scene_manager, HotspotArcadeSceneDashboard);
        return true;
    case HotspotArcadeMainItemGames:
        if(!app->snapshot.running) {
            hotspot_arcade_show_message(
                app, "Session not running", "Start a session before selecting a game.");
        } else {
            scene_manager_next_scene(app->scene_manager, HotspotArcadeSceneGames);
        }
        return true;
    case HotspotArcadeMainItemLeaderboard:
        scene_manager_next_scene(app->scene_manager, HotspotArcadeSceneLeaderboard);
        return true;
    case HotspotArcadeMainItemConsole:
        scene_manager_next_scene(app->scene_manager, HotspotArcadeSceneConsole);
        return true;
    case HotspotArcadeMainItemSsid:
        scene_manager_next_scene(app->scene_manager, HotspotArcadeSceneSsid);
        return true;
    case HotspotArcadeMainItemSettings:
        scene_manager_next_scene(app->scene_manager, HotspotArcadeSceneSettings);
        return true;
    default:
        return false;
    }
}

void hotspot_arcade_scene_main_on_exit(void* context) {
    HotspotArcadeApp* app = context;
    submenu_reset(app->submenu);
}
