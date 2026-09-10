#include "../ble_spam_app.h"
#include "../ble_keyboard.h"

#include <string.h>

enum {
    BleKeyboardInputDone = 0x500,
};

static void ble_keyboard_input_done_callback(void* context) {
    BleSpamApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, BleKeyboardInputDone);
}

void ble_spam_scene_ble_keyboard_input_on_enter(void* context) {
    BleSpamApp* app = context;
    TextInput* text_input = app->text_input;

    app->keyboard_text[0] = '\0';
    text_input_set_header_text(
        text_input,
        app->keyboard_append_enter ? "Type text, then send + Enter" :
                                     "Type text, then send");
    text_input_set_result_callback(
        text_input,
        ble_keyboard_input_done_callback,
        app,
        app->keyboard_text,
        sizeof(app->keyboard_text),
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewTextInput);
}

bool ble_spam_scene_ble_keyboard_input_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    if(event.type != SceneManagerEventTypeCustom || event.event != BleKeyboardInputDone) {
        return false;
    }

    if(app->keyboard_text[0] != '\0' || app->keyboard_append_enter) {
        ble_keyboard_send_text(app, app->keyboard_text, app->keyboard_append_enter);
    }
    app->keyboard_text[0] = '\0';
    scene_manager_previous_scene(app->scene_manager);
    return true;
}

void ble_spam_scene_ble_keyboard_input_on_exit(void* context) {
    BleSpamApp* app = context;
    text_input_reset(app->text_input);
    app->keyboard_input_active = false;
}
