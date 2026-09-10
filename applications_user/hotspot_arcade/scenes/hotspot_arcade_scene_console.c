#include "../hotspot_arcade_i.h"

static void hotspot_arcade_scene_console_button_callback(
    GuiButtonType button,
    InputType type,
    void* context) {
    HotspotArcadeApp* app = context;
    if(button == GuiButtonTypeCenter && type == InputTypeShort) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, HotspotArcadeEventWidgetAction);
    }
}

static void hotspot_arcade_scene_console_draw(HotspotArcadeApp* app) {
    const size_t copied = hotspot_arcade_service_copy_console(
        app->console_buffer, HOTSPOT_ARCADE_CONSOLE_BUFFER_SIZE);
    const size_t terminator =
        copied < HOTSPOT_ARCADE_CONSOLE_BUFFER_SIZE ? copied :
                                                       HOTSPOT_ARCADE_CONSOLE_BUFFER_SIZE - 1U;
    app->console_buffer[terminator] = '\0';
    app->console_buffer[HOTSPOT_ARCADE_CONSOLE_BUFFER_SIZE - 1U] = '\0';

    furi_string_set(app->display_text, "\e#Service console\n");
    if(app->console_buffer[0]) {
        furi_string_cat_str(app->display_text, app->console_buffer);
    } else {
        furi_string_cat_str(
            app->display_text,
            "\nNo service events yet. Start a session and connect a phone.");
    }

    widget_reset(app->widget);
    widget_add_button_element(
        app->widget,
        GuiButtonTypeCenter,
        "Refresh",
        hotspot_arcade_scene_console_button_callback,
        app);
    widget_add_text_scroll_element(
        app->widget, 0, 0, 128, 51, furi_string_get_cstr(app->display_text));
}

void hotspot_arcade_scene_console_on_enter(void* context) {
    HotspotArcadeApp* app = context;
    hotspot_arcade_scene_console_draw(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, HotspotArcadeViewWidget);
}

bool hotspot_arcade_scene_console_on_event(void* context, SceneManagerEvent event) {
    HotspotArcadeApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event == HotspotArcadeEventSnapshotChanged ||
       event.event == HotspotArcadeEventWidgetAction) {
        hotspot_arcade_scene_console_draw(app);
        return true;
    }
    return false;
}

void hotspot_arcade_scene_console_on_exit(void* context) {
    HotspotArcadeApp* app = context;
    widget_reset(app->widget);
}
