#include "../hotspot_arcade_i.h"

static void hotspot_arcade_scene_leaderboard_button_callback(
    GuiButtonType button,
    InputType type,
    void* context) {
    HotspotArcadeApp* app = context;
    if(button == GuiButtonTypeCenter && type == InputTypeShort) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, HotspotArcadeEventWidgetAction);
    }
}

static void hotspot_arcade_scene_leaderboard_draw(HotspotArcadeApp* app) {
    uint8_t order[HOTSPOT_ARCADE_SERVICE_MAX_PLAYERS] = {0};
    size_t count = 0U;
    for(uint8_t index = 0U; index < HOTSPOT_ARCADE_SERVICE_MAX_PLAYERS; index++) {
        if(app->snapshot.players[index].used) order[count++] = index;
    }

    for(size_t index = 1U; index < count; index++) {
        const uint8_t candidate = order[index];
        size_t position = index;
        while(position > 0U &&
              app->snapshot.players[order[position - 1U]].score <
                  app->snapshot.players[candidate].score) {
            order[position] = order[position - 1U];
            position--;
        }
        order[position] = candidate;
    }

    furi_string_set(app->display_text, "\e#Leaderboard\n");
    if(count == 0U) {
        furi_string_cat_str(app->display_text, "\nNo players have joined yet.");
    } else {
        for(size_t rank = 0U; rank < count; rank++) {
            const HotspotArcadeServicePlayer* player = &app->snapshot.players[order[rank]];
            furi_string_cat_printf(
                app->display_text,
                "%u. %s  %ld\n",
                (unsigned)(rank + 1U),
                player->nick[0] ? player->nick : "PLAYER",
                (long)player->score);
        }
    }

    widget_reset(app->widget);
    widget_add_button_element(
        app->widget,
        GuiButtonTypeCenter,
        "Reset",
        hotspot_arcade_scene_leaderboard_button_callback,
        app);
    widget_add_text_scroll_element(
        app->widget, 0, 0, 128, 51, furi_string_get_cstr(app->display_text));
}

void hotspot_arcade_scene_leaderboard_on_enter(void* context) {
    HotspotArcadeApp* app = context;
    hotspot_arcade_scene_leaderboard_draw(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, HotspotArcadeViewWidget);
}

bool hotspot_arcade_scene_leaderboard_on_event(void* context, SceneManagerEvent event) {
    HotspotArcadeApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == HotspotArcadeEventSnapshotChanged) {
        hotspot_arcade_scene_leaderboard_draw(app);
        return true;
    }
    if(event.event == HotspotArcadeEventWidgetAction) {
        if(!app->snapshot.running) {
            hotspot_arcade_show_message(
                app, "Session not running", "Start a session before resetting scores.");
        } else {
            hotspot_arcade_service_reset_scores();
            hotspot_arcade_feedback(app);
            hotspot_arcade_refresh_snapshot(app);
            hotspot_arcade_scene_leaderboard_draw(app);
        }
        return true;
    }
    return false;
}

void hotspot_arcade_scene_leaderboard_on_exit(void* context) {
    HotspotArcadeApp* app = context;
    widget_reset(app->widget);
}
