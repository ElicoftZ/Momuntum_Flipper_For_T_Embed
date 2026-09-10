#include "../spoofing_settings_app.h"
#include "spoofing_settings_scene.h"

#include <furi_hal_version.h>

enum TextInputIndex {
    TextInputResultOk,
};

static void spoofing_settings_scene_change_name_text_input_callback(void* context) {
    SpoofingSettingsApp* app = context;

    app->save_name = true;
    /* Persist to NVS + update RAM immediately so BLE/USB/passport pick up the
     * name even before the reboot (and without depending on SD-card storage). */
    furi_hal_version_set_name(app->device_name);
    view_dispatcher_send_custom_event(app->view_dispatcher, TextInputResultOk);
}

static bool spoofing_settings_scene_change_name_validator(
    const char* text,
    FuriString* error,
    void* context) {
    UNUSED(context);

    for(; *text; ++text) {
        const char c = *text;
        if((c < '0' || c > '9') && (c < 'A' || c > 'Z') && (c < 'a' || c > 'z')) {
            furi_string_printf(error, "Please only\nenter letters\nand numbers!");
            return false;
        }
    }

    return true;
}

void spoofing_settings_scene_change_name_on_enter(void* context) {
    SpoofingSettingsApp* app = context;
    TextInput* text_input = app->text_input;

    text_input_set_header_text(text_input, "Name (max 8 chars)");

    text_input_set_validator(text_input, spoofing_settings_scene_change_name_validator, NULL);

    text_input_set_minimum_length(text_input, 0);

    text_input_set_result_callback(
        text_input,
        spoofing_settings_scene_change_name_text_input_callback,
        app,
        app->device_name,
        FURI_HAL_VERSION_NAME_LENGTH + 1,
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, SpoofingSettingsAppViewTextInput);
}

bool spoofing_settings_scene_change_name_on_event(void* context, SceneManagerEvent event) {
    SpoofingSettingsApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;
        switch(event.event) {
        case TextInputResultOk:
            scene_manager_next_scene(app->scene_manager, SpoofingSettingsAppSceneNamePopup);
            break;
        default:
            break;
        }
    }

    return consumed;
}

void spoofing_settings_scene_change_name_on_exit(void* context) {
    SpoofingSettingsApp* app = context;
    text_input_reset(app->text_input);
}
