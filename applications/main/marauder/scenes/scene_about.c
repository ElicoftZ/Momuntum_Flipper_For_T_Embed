#include "../marauder_app_i.h"

static void marauder_scene_about_button_callback(GuiButtonType type, InputType input, void* context) {
    furi_assert(context);
    MarauderApp* app = context;
    if(input != InputTypeShort) return;
    if(type == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, 0);
    }
}

void marauder_scene_about_on_enter(void* context) {
    MarauderApp* app = context;

    widget_reset(app->widget);
    widget_add_text_box_element(
        app->widget,
        0,
        0,
        128,
        48,
        AlignCenter,
        AlignTop,
        "\e#Marauder\e#\n"
        "This menu launches this port's own WiFi/BLE apps, plus\n"
        "PMKID capture and probe-request flood, both new here.\n"
        "Concept + naming credited to ESP32Marauder by\n"
        "justcallmekoko -- see NOTICE in this app's folder.",
        false);
    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Back", marauder_scene_about_button_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, MarauderAppViewWidget);
}

bool marauder_scene_about_on_event(void* context, SceneManagerEvent event) {
    MarauderApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void marauder_scene_about_on_exit(void* context) {
    MarauderApp* app = context;
    widget_reset(app->widget);
}
