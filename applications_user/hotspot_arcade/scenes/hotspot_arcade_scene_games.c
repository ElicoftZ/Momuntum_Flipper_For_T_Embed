#include "../hotspot_arcade_i.h"

#define HOTSPOT_ARCADE_GAME_EVENT_BASE 0x2000U
#define HOTSPOT_ARCADE_GAME_COUNT 40U

static void hotspot_arcade_scene_games_submenu_callback(void* context, uint32_t index) {
    HotspotArcadeApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void hotspot_arcade_scene_games_on_enter(void* context) {
    HotspotArcadeApp* app = context;
    submenu_set_header(app->submenu, "Choose a game");
    for(uint8_t game_id = 1U; game_id <= HOTSPOT_ARCADE_GAME_COUNT; game_id++) {
        submenu_add_item(
            app->submenu,
            hotspot_arcade_game_name(game_id),
            HOTSPOT_ARCADE_GAME_EVENT_BASE + game_id,
            hotspot_arcade_scene_games_submenu_callback,
            app);
    }

    uint32_t selected =
        scene_manager_get_scene_state(app->scene_manager, HotspotArcadeSceneGames);
    if(selected < 1U || selected > HOTSPOT_ARCADE_GAME_COUNT) {
        selected = (app->snapshot.active_game >= 1U &&
                    app->snapshot.active_game <= HOTSPOT_ARCADE_GAME_COUNT) ?
                       app->snapshot.active_game :
                       1U;
    }
    submenu_set_selected_item(app->submenu, HOTSPOT_ARCADE_GAME_EVENT_BASE + selected);
    view_dispatcher_switch_to_view(app->view_dispatcher, HotspotArcadeViewSubmenu);
}

bool hotspot_arcade_scene_games_on_event(void* context, SceneManagerEvent event) {
    HotspotArcadeApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == HotspotArcadeEventSnapshotChanged) {
        if(!app->snapshot.running) {
            hotspot_arcade_show_message(
                app, "Session stopped", "Start a session before selecting a game.");
        }
        return true;
    }

    if(event.event <= HOTSPOT_ARCADE_GAME_EVENT_BASE ||
       event.event > HOTSPOT_ARCADE_GAME_EVENT_BASE + HOTSPOT_ARCADE_GAME_COUNT) {
        return false;
    }

    const uint8_t game_id = (uint8_t)(event.event - HOTSPOT_ARCADE_GAME_EVENT_BASE);
    scene_manager_set_scene_state(app->scene_manager, HotspotArcadeSceneGames, game_id);
    const HotspotArcadeServiceResult result = hotspot_arcade_service_select_game(game_id);
    if(result != HotspotArcadeServiceResultOk) {
        const char* reason = hotspot_arcade_service_result_to_string(result);
        furi_string_printf(
            app->display_text,
            "Could not select %s.\n\nReason: %s",
            hotspot_arcade_game_name(game_id),
            reason ? reason : "unknown error");
        hotspot_arcade_show_message(
            app, "Game selection failed", furi_string_get_cstr(app->display_text));
        return true;
    }

    hotspot_arcade_refresh_snapshot(app);
    hotspot_arcade_feedback(app);
    scene_manager_next_scene(app->scene_manager, HotspotArcadeSceneDashboard);
    return true;
}

void hotspot_arcade_scene_games_on_exit(void* context) {
    HotspotArcadeApp* app = context;
    submenu_reset(app->submenu);
}
