#include "../ble_spam_app.h"
#include "../ble_spam_lovespouse.h"

enum {
    LovespouseMenuRandom,
    LovespouseCustomEventDone = 100,
};

static void lovespouse_menu_callback(void* context, uint32_t index) {
    BleSpamApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void ble_spam_scene_lovespouse_config_on_enter(void* context) {
    BleSpamApp* app = context;
    const size_t mode_count = ble_spam_lovespouse_mode_count();

    submenu_add_item(
        app->submenu, "Random", LovespouseMenuRandom, lovespouse_menu_callback, app);
    for(size_t i = 0; i < mode_count; i++) {
        const BleSpamLovespouseMode* mode = ble_spam_lovespouse_mode_at(i);
        submenu_add_item(app->submenu, mode->name, i + 1, lovespouse_menu_callback, app);
    }
    submenu_add_item(app->submenu, "Custom", mode_count + 1, lovespouse_menu_callback, app);
    submenu_add_item(
        app->submenu, "Bruteforce", mode_count + 2, lovespouse_menu_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewSubmenu);
}

bool ble_spam_scene_lovespouse_config_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    const size_t mode_count = ble_spam_lovespouse_mode_count();
    if(event.event == LovespouseMenuRandom) {
        app->lovespouse_selection = BleSpamLovespouseRandom;
        scene_manager_next_scene(app->scene_manager, BleSpamSceneRunning);
    } else if(event.event <= mode_count) {
        const BleSpamLovespouseMode* mode = ble_spam_lovespouse_mode_at(event.event - 1);
        app->lovespouse_selection = BleSpamLovespouseValue;
        app->lovespouse_value = mode->value;
        scene_manager_next_scene(app->scene_manager, BleSpamSceneRunning);
    } else if(event.event == mode_count + 1) {
        scene_manager_next_scene(app->scene_manager, BleSpamSceneLovespouseCustom);
    } else if(event.event == mode_count + 2) {
        app->lovespouse_selection = BleSpamLovespouseBruteforce;
        app->lovespouse_value = 0;
        scene_manager_next_scene(app->scene_manager, BleSpamSceneRunning);
    } else {
        return false;
    }

    return true;
}

void ble_spam_scene_lovespouse_config_on_exit(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);
}

static void lovespouse_custom_callback(void* context) {
    BleSpamApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, LovespouseCustomEventDone);
}

void ble_spam_scene_lovespouse_custom_on_enter(void* context) {
    BleSpamApp* app = context;
    app->lovespouse_custom_value[0] = (uint8_t)(app->lovespouse_value >> 16);
    app->lovespouse_custom_value[1] = (uint8_t)(app->lovespouse_value >> 8);
    app->lovespouse_custom_value[2] = (uint8_t)app->lovespouse_value;

    byte_input_set_header_text(app->byte_input, "Enter custom mode");
    byte_input_set_result_callback(
        app->byte_input,
        lovespouse_custom_callback,
        NULL,
        app,
        app->lovespouse_custom_value,
        sizeof(app->lovespouse_custom_value));
    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewByteInput);
}

bool ble_spam_scene_lovespouse_custom_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    if(event.type != SceneManagerEventTypeCustom ||
       event.event != LovespouseCustomEventDone) {
        return false;
    }

    app->lovespouse_selection = BleSpamLovespouseValue;
    app->lovespouse_value = ((uint32_t)app->lovespouse_custom_value[0] << 16) |
                            ((uint32_t)app->lovespouse_custom_value[1] << 8) |
                            app->lovespouse_custom_value[2];
    scene_manager_next_scene(app->scene_manager, BleSpamSceneRunning);
    return true;
}

void ble_spam_scene_lovespouse_custom_on_exit(void* context) {
    BleSpamApp* app = context;
    byte_input_set_result_callback(app->byte_input, NULL, NULL, NULL, NULL, 0);
    byte_input_set_header_text(app->byte_input, "");
}
