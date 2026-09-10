#include "ble_keyboard.h"

#include <furi_hal_bt.h>
#include <furi_hal_usb_hid.h>
#include <string.h>

#define TAG "BleKeyboard"
#define BLE_KEYBOARD_KEY_DELAY_MS 6U

static void ble_keyboard_connection_callback(bool connected, void* context) {
    BleSpamApp* app = context;
    if(app) {
        app->keyboard_connected = connected;
    }
}

bool ble_keyboard_start(BleSpamApp* app) {
    furi_check(app);

    if(app->keyboard_hid_instance) {
        return true;
    }

    memset(&app->keyboard_hid_config, 0, sizeof(app->keyboard_hid_config));
    strlcpy(
        app->keyboard_hid_config.ble.name,
        "Momentum BLE Remote",
        sizeof(app->keyboard_hid_config.ble.name));
    app->keyboard_hid_config.ble.bonding = true;
    app->keyboard_hid_config.ble.pairing = GapPairingPinCodeVerifyYesNo;

    app->keyboard_hid = bad_usb_hid_get_interface(BadUsbHidInterfaceBle);
    app->keyboard_hid_instance = app->keyboard_hid->init(&app->keyboard_hid_config);
    if(!app->keyboard_hid_instance) {
        FURI_LOG_E(TAG, "Failed to start BLE HID profile");
        app->keyboard_hid = NULL;
        app->keyboard_connected = false;
        return false;
    }

    app->keyboard_hid->set_state_callback(
        app->keyboard_hid_instance, ble_keyboard_connection_callback, app);
    furi_hal_bt_start_advertising();
    FURI_LOG_I(TAG, "BLE HID profile started as Momentum BLE Remote");
    return true;
}

void ble_keyboard_stop(BleSpamApp* app) {
    if(!app || !app->keyboard_hid_instance || !app->keyboard_hid) {
        return;
    }

    app->keyboard_hid->set_state_callback(app->keyboard_hid_instance, NULL, NULL);
    app->keyboard_hid->release_all(app->keyboard_hid_instance);
    app->keyboard_hid->deinit(app->keyboard_hid_instance);
    app->keyboard_hid_instance = NULL;
    app->keyboard_hid = NULL;
    app->keyboard_connected = false;
    FURI_LOG_I(TAG, "BLE HID profile stopped; default profile restored");
}

bool ble_keyboard_send_key(BleSpamApp* app, uint16_t keycode) {
    if(!app || !app->keyboard_hid || !app->keyboard_hid_instance ||
       !app->keyboard_connected || keycode == HID_KEYBOARD_NONE) {
        return false;
    }

    const bool pressed = app->keyboard_hid->kb_press(app->keyboard_hid_instance, keycode);
    furi_delay_ms(BLE_KEYBOARD_KEY_DELAY_MS);
    const bool released = app->keyboard_hid->kb_release(app->keyboard_hid_instance, keycode);
    furi_delay_ms(BLE_KEYBOARD_KEY_DELAY_MS);
    return pressed && released;
}

bool ble_keyboard_send_consumer_key(BleSpamApp* app, uint16_t keycode) {
    if(!app || !app->keyboard_hid || !app->keyboard_hid_instance ||
       !app->keyboard_connected) {
        return false;
    }

    const bool pressed = app->keyboard_hid->consumer_press(app->keyboard_hid_instance, keycode);
    furi_delay_ms(BLE_KEYBOARD_KEY_DELAY_MS);
    const bool released =
        app->keyboard_hid->consumer_release(app->keyboard_hid_instance, keycode);
    furi_delay_ms(BLE_KEYBOARD_KEY_DELAY_MS);
    return pressed && released;
}

bool ble_keyboard_mouse_move(BleSpamApp* app, int8_t dx, int8_t dy) {
    if(!app || !app->keyboard_hid || !app->keyboard_hid_instance ||
       !app->keyboard_connected) {
        return false;
    }

    return app->keyboard_hid->mouse_move(app->keyboard_hid_instance, dx, dy);
}

bool ble_keyboard_mouse_click(BleSpamApp* app, uint8_t button) {
    if(!app || !app->keyboard_hid || !app->keyboard_hid_instance ||
       !app->keyboard_connected) {
        return false;
    }

    const bool pressed = app->keyboard_hid->mouse_press(app->keyboard_hid_instance, button);
    furi_delay_ms(BLE_KEYBOARD_KEY_DELAY_MS);
    const bool released = app->keyboard_hid->mouse_release(app->keyboard_hid_instance, button);
    furi_delay_ms(BLE_KEYBOARD_KEY_DELAY_MS);
    return pressed && released;
}

bool ble_keyboard_send_text(BleSpamApp* app, const char* text, bool append_enter) {
    if(!app || !text || !app->keyboard_connected) {
        return false;
    }

    bool sent_any = false;
    for(const uint8_t* cursor = (const uint8_t*)text; *cursor; cursor++) {
        const uint16_t keycode = HID_ASCII_TO_KEY(*cursor);
        if(keycode == HID_KEYBOARD_NONE) {
            continue; // Skip unsupported UTF-8/control bytes safely.
        }
        if(!ble_keyboard_send_key(app, keycode)) {
            return false;
        }
        sent_any = true;
    }

    if(append_enter && !ble_keyboard_send_key(app, HID_KEYBOARD_RETURN)) {
        return false;
    }

    return sent_any || append_enter;
}

void ble_keyboard_remove_pairing(BleSpamApp* app) {
    if(!app || !app->keyboard_hid_instance) {
        return;
    }

    if(app->keyboard_hid) {
        app->keyboard_hid->release_all(app->keyboard_hid_instance);
    }
    bad_usb_hid_ble_remove_pairing();
    app->keyboard_connected = false;
    furi_hal_bt_start_advertising();
}
