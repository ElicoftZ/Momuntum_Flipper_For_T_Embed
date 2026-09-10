#include "../hotspot_arcade_i.h"

static void hotspot_arcade_scene_dashboard_button_callback(
    GuiButtonType button,
    InputType type,
    void* context) {
    HotspotArcadeApp* app = context;
    if(button == GuiButtonTypeCenter && type == InputTypeShort) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, HotspotArcadeEventWidgetAction);
    }
}

static void hotspot_arcade_scene_dashboard_draw(HotspotArcadeApp* app) {
    widget_reset(app->widget);
    furi_string_reset(app->display_text);

    if(!app->snapshot.running) {
        furi_string_set(
            app->display_text,
            "\e#Session stopped\n\nStart the S3 hotspot from the main menu.");
        widget_add_button_element(
            app->widget,
            GuiButtonTypeCenter,
            "Back",
            hotspot_arcade_scene_dashboard_button_callback,
            app);
    } else {
        furi_string_printf(
            app->display_text,
            "\e#Hotspot Arcade\nIP: %s\nSSID: %s\nGame: %s\nPlayers: %u  Phones: %u\n\n%s",
            app->snapshot.ip[0] ? app->snapshot.ip : "starting...",
            app->snapshot.ssid[0] ? app->snapshot.ssid : app->settings.ssid,
            hotspot_arcade_game_name(app->snapshot.active_game),
            app->snapshot.player_count,
            app->snapshot.client_count,
            app->snapshot.last_event[0] ? app->snapshot.last_event : "Waiting for players");
        widget_add_button_element(
            app->widget,
            GuiButtonTypeCenter,
            "End Round",
            hotspot_arcade_scene_dashboard_button_callback,
            app);
    }
    widget_add_text_scroll_element(
        app->widget, 0, 0, 128, 51, furi_string_get_cstr(app->display_text));
}

void hotspot_arcade_scene_dashboard_on_enter(void* context) {
    HotspotArcadeApp* app = context;
    hotspot_arcade_scene_dashboard_draw(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, HotspotArcadeViewWidget);
}

bool hotspot_arcade_scene_dashboard_on_event(void* context, SceneManagerEvent event) {
    HotspotArcadeApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == HotspotArcadeEventSnapshotChanged) {
        hotspot_arcade_scene_dashboard_draw(app);
        return true;
    }
    if(event.event == HotspotArcadeEventWidgetAction) {
        if(app->snapshot.running) {
            hotspot_arcade_service_round_end();
            hotspot_arcade_feedback(app);
            hotspot_arcade_refresh_snapshot(app);
            hotspot_arcade_scene_dashboard_draw(app);
        } else {
            scene_manager_previous_scene(app->scene_manager);
        }
        return true;
    }
    return false;
}

void hotspot_arcade_scene_dashboard_on_exit(void* context) {
    HotspotArcadeApp* app = context;
    widget_reset(app->widget);
}
