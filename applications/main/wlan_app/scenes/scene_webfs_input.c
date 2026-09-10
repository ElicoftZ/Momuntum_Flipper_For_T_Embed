/* SSID / password entry (TextInput), reused for both fields via scene state
 * (0 = SSID, 1 = password). Persisted to /ext/webfs/config.txt on OK. */

#include "../wlan_app.h"

#include <string.h>

static void webfs_input_result_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventWebFsInputDone);
}

/* WPA2 needs >= 8 chars; empty = open AP. Reject anything in between. */
static bool webfs_pw_validator(const char* text, FuriString* error, void* ctx) {
    UNUSED(ctx);
    size_t n = strlen(text);
    if(n == 0 || n >= 8) return true;
    furi_string_set(error, "Min. 8 chars\nor leave empty");
    return false;
}

void wlan_app_scene_webfs_input_on_enter(void* context) {
    WlanApp* app = context;
    TextInput* ti = app->text_input;
    uint32_t target = scene_manager_get_scene_state(app->scene_manager, WlanAppSceneWebFsInput);

    text_input_reset(ti);
    text_input_set_validator(ti, NULL, NULL);

    if(target == 0) {
        text_input_set_header_text(ti, "SSID");
        text_input_set_minimum_length(ti, 1);
        text_input_set_result_callback(
            ti, webfs_input_result_cb, app, app->webfs_ssid, WLAN_WEBFS_SSID_MAX + 1, false);
    } else {
        text_input_set_header_text(ti, "Password (>=8 / empty=open)");
        text_input_set_minimum_length(ti, 0);
        text_input_set_validator(ti, webfs_pw_validator, NULL);
        text_input_set_result_callback(
            ti, webfs_input_result_cb, app, app->webfs_pw, WLAN_WEBFS_PW_MAX + 1, false);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewTextInput);
}

bool wlan_app_scene_webfs_input_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventWebFsInputDone) {
            wlan_webfs_config_save(app->webfs_ssid, app->webfs_pw);
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }
    return consumed;
}

void wlan_app_scene_webfs_input_on_exit(void* context) {
    WlanApp* app = context;
    text_input_reset(app->text_input);
}
