#include "../spoofing_settings_app.h"
#include "spoofing_settings_scene.h"

enum SubmenuIndex {
    SubmenuIndexFlipperName,
    SubmenuIndexShellColor,
};

static void spoofing_settings_scene_start_submenu_callback(void* context, uint32_t index) {
    SpoofingSettingsApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void spoofing_settings_scene_start_on_enter(void* context) {
    SpoofingSettingsApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_add_item(
        submenu,
        "Flipper Name",
        SubmenuIndexFlipperName,
        spoofing_settings_scene_start_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "Shell Color",
        SubmenuIndexShellColor,
        spoofing_settings_scene_start_submenu_callback,
        app);

    view_dispatcher_switch_to_view(app->view_dispatcher, SpoofingSettingsAppViewMenu);
}

bool spoofing_settings_scene_start_on_event(void* context, SceneManagerEvent event) {
    SpoofingSettingsApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubmenuIndexFlipperName) {
            scene_manager_next_scene(app->scene_manager, SpoofingSettingsAppSceneChangeName);
            consumed = true;
        } else if(event.event == SubmenuIndexShellColor) {
            scene_manager_next_scene(app->scene_manager, SpoofingSettingsAppSceneShellColor);
            consumed = true;
        }
    }

    return consumed;
}

void spoofing_settings_scene_start_on_exit(void* context) {
    SpoofingSettingsApp* app = context;
    submenu_reset(app->submenu);
}
