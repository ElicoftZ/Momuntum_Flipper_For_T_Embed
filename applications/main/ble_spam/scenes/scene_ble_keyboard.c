#include "../ble_spam_app.h"
#include "../ble_keyboard.h"

#include <furi_hal_usb_hid.h>

enum BleKeyboardMenuIndex {
    BleKeyboardMenuStatus,
    BleKeyboardMenuTypeSend,
    BleKeyboardMenuTypeSendEnter,
    BleKeyboardMenuEnter,
    BleKeyboardMenuBackspace,
    BleKeyboardMenuTab,
    BleKeyboardMenuEscape,
    BleKeyboardMenuArrowUp,
    BleKeyboardMenuArrowDown,
    BleKeyboardMenuArrowLeft,
    BleKeyboardMenuArrowRight,
    BleKeyboardMenuCopy,
    BleKeyboardMenuPaste,
    BleKeyboardMenuSelectAll,
    BleKeyboardMenuUnpair,
};

static void ble_keyboard_menu_callback(void* context, uint32_t index) {
    BleSpamApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void ble_keyboard_update_status(BleSpamApp* app) {
    const char* label;
    if(!app->keyboard_hid_instance) {
        label = "Status: unavailable";
        submenu_set_header(app->submenu, "BLE Keyboard failed to start");
    } else if(app->keyboard_connected) {
        label = "Status: connected";
        submenu_set_header(
            app->submenu, app->hid_no_sleep ? "BLE Keyboard | No sleep" : "BLE Keyboard");
    } else {
        label = "Status: pair Momentum BLE Remote";
        submenu_set_header(
            app->submenu, app->hid_no_sleep ? "BLE Keyboard | No sleep" : "BLE Keyboard");
    }
    submenu_change_item_label(app->submenu, BleKeyboardMenuStatus, label);
}

static bool ble_keyboard_handle_key(BleSpamApp* app, uint16_t keycode) {
    const bool sent = ble_keyboard_send_key(app, keycode);
    ble_keyboard_update_status(app);
    return sent;
}

void ble_spam_scene_ble_keyboard_on_enter(void* context) {
    BleSpamApp* app = context;

    ble_spam_hid_wake_start(app);
    app->keyboard_input_active = false;
    const bool started = ble_keyboard_start(app);
    submenu_set_header(
        app->submenu, started ? "BLE Keyboard" : "BLE Keyboard failed to start");
    submenu_add_item(
        app->submenu, "Status", BleKeyboardMenuStatus, NULL, NULL);
    submenu_add_item(
        app->submenu,
        "Type & Send",
        BleKeyboardMenuTypeSend,
        ble_keyboard_menu_callback,
        app);
    submenu_add_item(
        app->submenu,
        "Type + Enter",
        BleKeyboardMenuTypeSendEnter,
        ble_keyboard_menu_callback,
        app);
    submenu_add_item(
        app->submenu, "Enter", BleKeyboardMenuEnter, ble_keyboard_menu_callback, app);
    submenu_add_item(
        app->submenu, "Backspace", BleKeyboardMenuBackspace, ble_keyboard_menu_callback, app);
    submenu_add_item(app->submenu, "Tab", BleKeyboardMenuTab, ble_keyboard_menu_callback, app);
    submenu_add_item(
        app->submenu, "Escape", BleKeyboardMenuEscape, ble_keyboard_menu_callback, app);
    submenu_add_item(
        app->submenu, "Arrow Up", BleKeyboardMenuArrowUp, ble_keyboard_menu_callback, app);
    submenu_add_item(
        app->submenu, "Arrow Down", BleKeyboardMenuArrowDown, ble_keyboard_menu_callback, app);
    submenu_add_item(
        app->submenu, "Arrow Left", BleKeyboardMenuArrowLeft, ble_keyboard_menu_callback, app);
    submenu_add_item(
        app->submenu, "Arrow Right", BleKeyboardMenuArrowRight, ble_keyboard_menu_callback, app);
    submenu_add_item(
        app->submenu, "Ctrl+C", BleKeyboardMenuCopy, ble_keyboard_menu_callback, app);
    submenu_add_item(
        app->submenu, "Ctrl+V", BleKeyboardMenuPaste, ble_keyboard_menu_callback, app);
    submenu_add_item(
        app->submenu, "Ctrl+A", BleKeyboardMenuSelectAll, ble_keyboard_menu_callback, app);
    submenu_add_item(
        app->submenu,
        "Remove Pairing",
        BleKeyboardMenuUnpair,
        ble_keyboard_menu_callback,
        app);

    ble_keyboard_update_status(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewSubmenu);
}

bool ble_spam_scene_ble_keyboard_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;

    if(event.type == SceneManagerEventTypeTick) {
        ble_keyboard_update_status(app);
        return true;
    }

    if(event.type != SceneManagerEventTypeCustom) {
        return false;
    }

    switch(event.event) {
    case BleKeyboardMenuTypeSend:
    case BleKeyboardMenuTypeSendEnter:
        app->keyboard_append_enter = (event.event == BleKeyboardMenuTypeSendEnter);
        app->keyboard_input_active = true;
        scene_manager_next_scene(app->scene_manager, BleSpamSceneBleKeyboardInput);
        break;
    case BleKeyboardMenuEnter:
        ble_keyboard_handle_key(app, HID_KEYBOARD_RETURN);
        break;
    case BleKeyboardMenuBackspace:
        ble_keyboard_handle_key(app, HID_KEYBOARD_DELETE);
        break;
    case BleKeyboardMenuTab:
        ble_keyboard_handle_key(app, HID_KEYBOARD_TAB);
        break;
    case BleKeyboardMenuEscape:
        ble_keyboard_handle_key(app, HID_KEYBOARD_ESCAPE);
        break;
    case BleKeyboardMenuArrowUp:
        ble_keyboard_handle_key(app, HID_KEYBOARD_UP_ARROW);
        break;
    case BleKeyboardMenuArrowDown:
        ble_keyboard_handle_key(app, HID_KEYBOARD_DOWN_ARROW);
        break;
    case BleKeyboardMenuArrowLeft:
        ble_keyboard_handle_key(app, HID_KEYBOARD_LEFT_ARROW);
        break;
    case BleKeyboardMenuArrowRight:
        ble_keyboard_handle_key(app, HID_KEYBOARD_RIGHT_ARROW);
        break;
    case BleKeyboardMenuCopy:
        ble_keyboard_handle_key(app, KEY_MOD_LEFT_CTRL | HID_KEYBOARD_C);
        break;
    case BleKeyboardMenuPaste:
        ble_keyboard_handle_key(app, KEY_MOD_LEFT_CTRL | HID_KEYBOARD_V);
        break;
    case BleKeyboardMenuSelectAll:
        ble_keyboard_handle_key(app, KEY_MOD_LEFT_CTRL | HID_KEYBOARD_A);
        break;
    case BleKeyboardMenuUnpair:
        ble_keyboard_remove_pairing(app);
        ble_keyboard_update_status(app);
        break;
    default:
        break;
    }

    return true;
}

void ble_spam_scene_ble_keyboard_on_exit(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);

    // Keep the profile only across the compose-text child scene. Returning to
    // the Bluetooth menu restores the default profile before another BLE tool
    // can take ownership of the controller.
    if(!app->keyboard_input_active) {
        ble_spam_hid_wake_stop(app);
        ble_keyboard_stop(app);
    }
}
