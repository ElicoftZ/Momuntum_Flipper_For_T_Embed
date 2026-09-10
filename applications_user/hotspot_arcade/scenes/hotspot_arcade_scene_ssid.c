#include "../hotspot_arcade_i.h"

static void hotspot_arcade_scene_ssid_copy(
    char* destination,
    size_t capacity,
    const char* source) {
    size_t index = 0U;
    while(index + 1U < capacity && source[index]) {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

static void hotspot_arcade_scene_ssid_result_callback(void* context) {
    HotspotArcadeApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, HotspotArcadeEventTextInputDone);
}

void hotspot_arcade_scene_ssid_on_enter(void* context) {
    HotspotArcadeApp* app = context;
    hotspot_arcade_scene_ssid_copy(
        app->ssid_edit, sizeof(app->ssid_edit), app->settings.ssid);
    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, "Hotspot name (SSID)");
    text_input_set_minimum_length(app->text_input, 1U);
    text_input_set_result_callback(
        app->text_input,
        hotspot_arcade_scene_ssid_result_callback,
        app,
        app->ssid_edit,
        sizeof(app->ssid_edit),
        false);
    view_dispatcher_switch_to_view(app->view_dispatcher, HotspotArcadeViewTextInput);
}

bool hotspot_arcade_scene_ssid_on_event(void* context, SceneManagerEvent event) {
    HotspotArcadeApp* app = context;
    if(event.type != SceneManagerEventTypeCustom ||
       event.event != HotspotArcadeEventTextInputDone) {
        return false;
    }

    hotspot_arcade_scene_ssid_copy(
        app->settings.ssid, sizeof(app->settings.ssid), app->ssid_edit);
    if(!hotspot_arcade_settings_save(app->storage, &app->settings)) {
        hotspot_arcade_show_message(
            app,
            "Could not save SSID",
            "Check that the SD card is mounted and writable.");
        return true;
    }

    hotspot_arcade_feedback(app);
    scene_manager_previous_scene(app->scene_manager);
    hotspot_arcade_show_message(
        app,
        "SSID saved",
        app->snapshot.running ?
            "The new hotspot name will be used the next time the session starts." :
            "The new hotspot name will be used when the session starts.");
    return true;
}

void hotspot_arcade_scene_ssid_on_exit(void* context) {
    HotspotArcadeApp* app = context;
    text_input_reset(app->text_input);
}
