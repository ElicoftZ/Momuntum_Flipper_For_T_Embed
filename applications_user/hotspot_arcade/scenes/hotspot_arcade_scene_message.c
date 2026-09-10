#include "../hotspot_arcade_i.h"

static void hotspot_arcade_scene_message_button_callback(
    GuiButtonType button,
    InputType type,
    void* context) {
    HotspotArcadeApp* app = context;
    if(button == GuiButtonTypeCenter && type == InputTypeShort) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, HotspotArcadeEventWidgetAction);
    }
}

void hotspot_arcade_scene_message_on_enter(void* context) {
    HotspotArcadeApp* app = context;
    widget_add_text_box_element(
        app->widget,
        0,
        0,
        128,
        14,
        AlignCenter,
        AlignCenter,
        furi_string_get_cstr(app->message_title),
        true);
    widget_add_text_scroll_element(
        app->widget, 0, 15, 128, 36, furi_string_get_cstr(app->message_body));
    widget_add_button_element(
        app->widget,
        GuiButtonTypeCenter,
        "Back",
        hotspot_arcade_scene_message_button_callback,
        app);
    view_dispatcher_switch_to_view(app->view_dispatcher, HotspotArcadeViewWidget);
}

bool hotspot_arcade_scene_message_on_event(void* context, SceneManagerEvent event) {
    HotspotArcadeApp* app = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == HotspotArcadeEventWidgetAction) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == HotspotArcadeEventSnapshotChanged) {
        return true;
    }
    return false;
}

void hotspot_arcade_scene_message_on_exit(void* context) {
    HotspotArcadeApp* app = context;
    widget_reset(app->widget);
}
