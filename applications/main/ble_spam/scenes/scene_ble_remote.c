#include "../ble_spam_app.h"
#include "../ble_keyboard.h"

#include <furi_hal_usb_hid.h>

#define BLE_REMOTE_JIGGLE_INTERVAL_MS 10000U
#define BLE_REMOTE_MOUSE_STEP         12

static void ble_remote_refresh(BleSpamApp* app) {
    BleRemoteModel* model = view_get_model(app->view_remote);
    model->mode = app->remote_mode;
    model->connected = app->keyboard_connected;
    model->active = app->remote_active;
    model->no_sleep = app->hid_no_sleep;
    view_commit_model(app->view_remote, true);
}

static bool ble_remote_send_consumer(BleSpamApp* app, uint16_t keycode) {
    return ble_keyboard_send_consumer_key(app, keycode);
}

static void ble_remote_handle_input(BleSpamApp* app, uint32_t event) {
    switch(app->remote_mode) {
    case BleRemoteModePresenter:
        if(event == BleRemoteEventUp) {
            ble_keyboard_send_key(app, HID_KEYBOARD_PAGE_UP);
        } else if(event == BleRemoteEventDown) {
            ble_keyboard_send_key(app, HID_KEYBOARD_PAGE_DOWN);
        } else if(event == BleRemoteEventLeft) {
            ble_keyboard_send_key(app, HID_KEYBOARD_LEFT_ARROW);
        } else if(event == BleRemoteEventRight) {
            ble_keyboard_send_key(app, HID_KEYBOARD_RIGHT_ARROW);
        } else if(event == BleRemoteEventOk) {
            ble_keyboard_send_key(app, HID_KEYBOARD_SPACEBAR);
        }
        break;

    case BleRemoteModeMedia:
        if(event == BleRemoteEventUp) {
            ble_remote_send_consumer(app, HID_CONSUMER_SCAN_PREVIOUS_TRACK);
        } else if(event == BleRemoteEventDown) {
            ble_remote_send_consumer(app, HID_CONSUMER_SCAN_NEXT_TRACK);
        } else if(event == BleRemoteEventLeft) {
            ble_remote_send_consumer(app, HID_CONSUMER_VOLUME_DECREMENT);
        } else if(event == BleRemoteEventRight) {
            ble_remote_send_consumer(app, HID_CONSUMER_VOLUME_INCREMENT);
        } else if(event == BleRemoteEventOk) {
            ble_remote_send_consumer(app, HID_CONSUMER_PLAY_PAUSE);
        }
        break;

    case BleRemoteModeCamera:
        if(event == BleRemoteEventOk) {
            // Android and iOS camera apps commonly use a keyboard's volume-up
            // control as the shutter button.
            ble_remote_send_consumer(app, HID_CONSUMER_VOLUME_INCREMENT);
        }
        break;

    case BleRemoteModeMouse:
        if(event == BleRemoteEventUp) {
            ble_keyboard_mouse_move(app, 0, -BLE_REMOTE_MOUSE_STEP);
        } else if(event == BleRemoteEventDown) {
            ble_keyboard_mouse_move(app, 0, BLE_REMOTE_MOUSE_STEP);
        } else if(event == BleRemoteEventLeft) {
            ble_keyboard_mouse_move(app, -BLE_REMOTE_MOUSE_STEP, 0);
        } else if(event == BleRemoteEventRight) {
            ble_keyboard_mouse_move(app, BLE_REMOTE_MOUSE_STEP, 0);
        } else if(event == BleRemoteEventOk) {
            ble_keyboard_mouse_click(app, HID_MOUSE_BTN_LEFT);
        }
        break;

    case BleRemoteModeMouseJiggler:
        if(event == BleRemoteEventOk) {
            app->remote_active = !app->remote_active;
            app->remote_last_action_tick = furi_get_tick();
        }
        break;

    }
}

void ble_spam_scene_ble_remote_on_enter(void* context) {
    BleSpamApp* app = context;

    ble_spam_hid_wake_start(app);
    app->remote_active = false;
    app->remote_last_action_tick = furi_get_tick();
    app->remote_jiggle_direction = 1;
    ble_keyboard_start(app);

    ble_remote_refresh(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewRemote);
}

bool ble_spam_scene_ble_remote_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        ble_remote_handle_input(app, event.event);
        ble_remote_refresh(app);
        return true;
    }

    if(event.type == SceneManagerEventTypeTick) {
        if(app->remote_mode == BleRemoteModeMouseJiggler && app->remote_active &&
           app->keyboard_connected) {
            const uint32_t now = furi_get_tick();
            if((now - app->remote_last_action_tick) >=
               furi_ms_to_ticks(BLE_REMOTE_JIGGLE_INTERVAL_MS)) {
                ble_keyboard_mouse_move(app, app->remote_jiggle_direction * 3, 0);
                app->remote_jiggle_direction = -app->remote_jiggle_direction;
                app->remote_last_action_tick = now;
            }
        }
        ble_remote_refresh(app);
        return true;
    }

    return false;
}

void ble_spam_scene_ble_remote_on_exit(void* context) {
    BleSpamApp* app = context;
    app->remote_active = false;
    ble_spam_hid_wake_stop(app);
    ble_keyboard_stop(app);
}
