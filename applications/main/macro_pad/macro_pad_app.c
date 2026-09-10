#include "macro_model.h"

#include "../bad_usb/helpers/bad_usb_hid.h"
#include "../wifi/wlan_hal.h"

#include <assets_icons.h>
#include <dolphin/dolphin.h>
#include <furi.h>
#include <furi_hal_power.h>
#include <furi_hal_random.h>
#include <furi_hal_rtc.h>
#include <furi_hal_usb.h>
#include <gui/canvas.h>
#include <gui/gui.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/variable_item_list.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <toolbox/saved_struct.h>
#include <stdio.h>
#include <string.h>

#define TAG "MacroPad"
#define MACRO_PAD_KEY_DELAY_MS         (6U)
#define MACRO_PAD_FEEDBACK_MS          (350U)
#define MACRO_PAD_DOUBLE_PRESS_MS      (325U)
#define MACRO_PAD_AUTO_USB_SETTLE_MS   (1500U)
#define MACRO_PAD_DIM_TIMEOUT_MS       (5UL * 60UL * 1000UL)
#define MACRO_PAD_SLEEP_TIMEOUT_MS     (10UL * 60UL * 1000UL)
#define MACRO_PAD_USB_PRESENT_VOLTS    (4.0f)
#define MACRO_PAD_BLE_IDENTITY_PATH    APP_DATA_PATH(".ble_identity")
#define MACRO_PAD_BLE_IDENTITY_MAGIC   0x4D
#define MACRO_PAD_BLE_IDENTITY_VERSION 1

typedef struct {
    uint8_t mac[GAP_MAC_ADDR_SIZE];
} MacroPadBleIdentity;

typedef enum {
    MacroPadViewConfig,
    MacroPadViewGesture,
    MacroPadViewRun,
    MacroPadViewSequence,
    MacroPadViewDialog,
} MacroPadViewId;

typedef enum {
    MacroPadRowRun,
    MacroPadRowTransport,
    MacroPadRowProfile,
    MacroPadRowLed,
    MacroPadRowStayAwake,
    MacroPadRowCcw,
    MacroPadRowCw,
    MacroPadRowPress,
    MacroPadRowDoublePress,
    MacroPadRowLongPress,
    MacroPadRowCopyProfile,
    MacroPadRowPasteProfile,
    MacroPadRowExport,
    MacroPadRowImport,
    MacroPadRowRepair,
    MacroPadRowResetProfile,
} MacroPadRow;

typedef enum {
    MacroPadGestureRowCategory,
    MacroPadGestureRowAction,
    MacroPadGestureRowTest,
    MacroPadGestureRowAdvanced,
    MacroPadGestureRowCopy,
    MacroPadGestureRowPaste,
    MacroPadGestureRowCount,
} MacroPadGestureRow;

typedef enum {
    MacroPadActionCategoryEditing,
    MacroPadActionCategoryMedia,
    MacroPadActionCategoryNavigation,
    MacroPadActionCategoryMac,
    MacroPadActionCategoryAll,
    MacroPadActionCategoryCount,
} MacroPadActionCategory;

typedef enum {
    MacroPadSequenceRowStep1,
    MacroPadSequenceRowDelay1,
    MacroPadSequenceRowStep2,
    MacroPadSequenceRowDelay2,
    MacroPadSequenceRowStep3,
    MacroPadSequenceRowDelay3,
    MacroPadSequenceRowStep4,
    MacroPadSequenceRowCount,
} MacroPadSequenceRow;

typedef enum {
    MacroPadDialogNone,
    MacroPadDialogRepairConfirm,
    MacroPadDialogRepairResult,
    MacroPadDialogImportConfirm,
    MacroPadDialogProfilePasteConfirm,
    MacroPadDialogTransferResult,
} MacroPadDialogMode;

typedef enum {
    MacroPadEventRun = 0x800,
    MacroPadEventResetProfile,
    MacroPadEventExport,
    MacroPadEventImportOpen,
    MacroPadEventImportConfirm,
    MacroPadEventRepairOpen,
    MacroPadEventRepairConfirm,
    MacroPadEventProfileCopy,
    MacroPadEventProfilePasteOpen,
    MacroPadEventProfilePasteConfirm,
    MacroPadEventGestureTest,
    MacroPadEventGestureAdvanced,
    MacroPadEventGestureCopy,
    MacroPadEventGesturePaste,
    MacroPadEventDialogClose,
    MacroPadEventEditCcw,
    MacroPadEventEditCw,
    MacroPadEventEditPress,
    MacroPadEventEditDoublePress,
    MacroPadEventEditLongPress,
    MacroPadEventCcw,
    MacroPadEventCw,
    MacroPadEventPress,
    MacroPadEventDoublePress,
    MacroPadEventLongPress,
    MacroPadEventProfilePrevious,
    MacroPadEventProfileNext,
} MacroPadEvent;

typedef struct {
    uint8_t configured_transport;
    uint8_t active_transport;
    uint8_t profile;
    uint8_t first_actions[MACRO_PAD_GESTURE_COUNT];
    uint8_t action_counts[MACRO_PAD_GESTURE_COUNT];
    bool active_transport_valid;
    bool connected;
    bool start_failed;
    bool stay_awake;
    uint8_t active_gesture;
    uint8_t reconnect_phase;
} MacroPadViewModel;

typedef struct {
    Gui* gui;
    NotificationApp* notification;
    FuriPubSub* input_events;
    FuriPubSubSubscription* input_subscription;
    ViewDispatcher* view_dispatcher;
    VariableItemList* config_list;
    VariableItemList* gesture_list;
    VariableItemList* sequence_list;
    View* run_view;
    DialogEx* dialog;

    MacroPadSettings settings;
    bool settings_dirty;
    bool in_run;
    bool in_gesture;
    bool in_sequence;
    bool in_dialog;
    uint8_t edit_gesture;
    uint8_t edit_category;
    MacroPadDialogMode dialog_mode;
    MacroPadSequence gesture_clipboard;
    bool gesture_clipboard_valid;
    MacroPadProfile profile_clipboard;
    bool profile_clipboard_valid;
    uint8_t profile_clipboard_source;
    bool test_gesture_pending;
    bool test_return_to_gesture;
    uint8_t ble_mac[GAP_MAC_ADDR_SIZE];

    const BadUsbHidApi* hid;
    void* hid_instance;
    BadUsbHidConfig hid_config;
    FuriHalUsbInterface* usb_previous;
    volatile bool hid_connected;
    bool hid_started;
    bool transport_failed;
    uint8_t active_transport;
    bool active_transport_valid;
    bool auto_usb_rejected;
    bool auto_vbus_present;
    uint32_t auto_usb_deadline;

    volatile uint32_t wake_last_activity_tick;
    volatile bool wake_rearm_requested;
    bool wake_managed;
    bool wake_locked;
    bool insomnia_held;

    bool pending_press;
    uint32_t pending_press_tick;
    uint8_t last_action_gesture;
    uint32_t last_action_tick;
} MacroPadApp;

static const uint16_t macro_pad_delay_values[] = {0U, 25U, 50U, 100U, 250U, 500U, 1000U};
static const char* const macro_pad_delay_labels[] = {
    "0 ms",
    "25 ms",
    "50 ms",
    "100 ms",
    "250 ms",
    "500 ms",
    "1 sec",
};

static const char* const macro_pad_category_labels[MacroPadActionCategoryCount] = {
    [MacroPadActionCategoryEditing] = "Editing",
    [MacroPadActionCategoryMedia] = "Media",
    [MacroPadActionCategoryNavigation] = "Navigation",
    [MacroPadActionCategoryMac] = "macOS",
    [MacroPadActionCategoryAll] = "All Actions",
};

static const MacroPadAction macro_pad_editing_actions[] = {
    MacroPadActionNone,
    MacroPadActionCopy,
    MacroPadActionPaste,
    MacroPadActionUndo,
    MacroPadActionRedo,
    MacroPadActionSelectAll,
    MacroPadActionSave,
    MacroPadActionFind,
    MacroPadActionEnter,
    MacroPadActionEscape,
    MacroPadActionTab,
    MacroPadActionSpace,
    MacroPadActionBackspace,
    MacroPadActionScreenshot,
};

static const MacroPadAction macro_pad_media_actions[] = {
    MacroPadActionNone,
    MacroPadActionPlayPause,
    MacroPadActionMute,
    MacroPadActionVolumeUp,
    MacroPadActionVolumeDown,
    MacroPadActionNextTrack,
    MacroPadActionPreviousTrack,
};

static const MacroPadAction macro_pad_navigation_actions[] = {
    MacroPadActionNone,
    MacroPadActionEnter,
    MacroPadActionEscape,
    MacroPadActionTab,
    MacroPadActionSpace,
    MacroPadActionBackspace,
    MacroPadActionPageUp,
    MacroPadActionPageDown,
    MacroPadActionHome,
    MacroPadActionEnd,
    MacroPadActionBrowserBack,
    MacroPadActionBrowserForward,
};

static const MacroPadAction macro_pad_mac_actions[] = {
    MacroPadActionNone,
    MacroPadActionMacCopy,
    MacroPadActionMacPaste,
    MacroPadActionMacUndo,
    MacroPadActionMacRedo,
    MacroPadActionMacSelectAll,
    MacroPadActionMacSave,
    MacroPadActionMacFind,
    MacroPadActionEnter,
    MacroPadActionEscape,
    MacroPadActionTab,
    MacroPadActionSpace,
    MacroPadActionBackspace,
    MacroPadActionScreenshot,
};

static const char* const macro_pad_gesture_short_labels[MACRO_PAD_GESTURE_COUNT] = {
    [MacroPadGestureCcw] = "CCW",
    [MacroPadGestureCw] = "CW",
    [MacroPadGesturePress] = "Click",
    [MacroPadGestureDoublePress] = "Double",
    [MacroPadGestureLongPress] = "Hold",
};

static const char* const macro_pad_sequence_row_labels[MacroPadSequenceRowCount] = {
    [MacroPadSequenceRowStep1] = "Action 1",
    [MacroPadSequenceRowDelay1] = "Delay 1 -> 2",
    [MacroPadSequenceRowStep2] = "Action 2",
    [MacroPadSequenceRowDelay2] = "Delay 2 -> 3",
    [MacroPadSequenceRowStep3] = "Action 3",
    [MacroPadSequenceRowDelay3] = "Delay 3 -> 4",
    [MacroPadSequenceRowStep4] = "Action 4",
};

static const char* const macro_pad_editor_headers[MACRO_PAD_GESTURE_COUNT] = {
    [MacroPadGestureCcw] = "CCW Macro | OK edit",
    [MacroPadGestureCw] = "CW Macro | OK edit",
    [MacroPadGesturePress] = "Click Macro | OK edit",
    [MacroPadGestureDoublePress] = "Double Macro | OK edit",
    [MacroPadGestureLongPress] = "Hold Macro | OK edit",
};

static const NotificationSequence macro_pad_led_cyan = {
    &message_red_0,
    &message_green_255,
    &message_blue_255,
    &message_do_not_reset,
    NULL,
};

static const NotificationSequence macro_pad_led_magenta = {
    &message_red_255,
    &message_green_0,
    &message_blue_255,
    &message_do_not_reset,
    NULL,
};

static const NotificationSequence macro_pad_led_white = {
    &message_red_255,
    &message_green_255,
    &message_blue_255,
    &message_do_not_reset,
    NULL,
};

static void macro_pad_transport_stop(MacroPadApp* app);
static void macro_pad_update_config_items(MacroPadApp* app);
static void macro_pad_update_gesture_items(MacroPadApp* app);
static void macro_pad_update_sequence_items(MacroPadApp* app);
static MacroPadSequence* macro_pad_edit_sequence(MacroPadApp* app);

static bool macro_pad_ble_identity_valid(const uint8_t mac[GAP_MAC_ADDR_SIZE]) {
    bool any_byte = false;
    for(size_t i = 0; i < GAP_MAC_ADDR_SIZE; i++) {
        any_byte |= mac[i] != 0;
    }

    /* BadUSB stores this address reversed relative to the BLE HAL. */
    return any_byte && (mac[GAP_MAC_ADDR_SIZE - 1] & 0xC0U) == 0xC0U;
}

static void macro_pad_ble_identity_load(MacroPadApp* app) {
    MacroPadBleIdentity identity = {0};
    if(saved_struct_load(
           MACRO_PAD_BLE_IDENTITY_PATH,
           &identity,
           sizeof(identity),
           MACRO_PAD_BLE_IDENTITY_MAGIC,
           MACRO_PAD_BLE_IDENTITY_VERSION) &&
       macro_pad_ble_identity_valid(identity.mac)) {
        memcpy(app->ble_mac, identity.mac, sizeof(app->ble_mac));
    }
}

static bool macro_pad_ble_identity_renew(MacroPadApp* app) {
    MacroPadBleIdentity identity;
    furi_hal_random_fill_buf(identity.mac, sizeof(identity.mac));
    identity.mac[GAP_MAC_ADDR_SIZE - 1] |= 0xC0U;

    if(!saved_struct_save(
           MACRO_PAD_BLE_IDENTITY_PATH,
           &identity,
           sizeof(identity),
           MACRO_PAD_BLE_IDENTITY_MAGIC,
           MACRO_PAD_BLE_IDENTITY_VERSION)) {
        FURI_LOG_E(TAG, "Could not save renewed BLE identity");
        return false;
    }

    memcpy(app->ble_mac, identity.mac, sizeof(app->ble_mac));
    FURI_LOG_I(
        TAG,
        "BLE identity renewed: %02x:%02x:%02x:%02x:%02x:%02x",
        app->ble_mac[0],
        app->ble_mac[1],
        app->ble_mac[2],
        app->ble_mac[3],
        app->ble_mac[4],
        app->ble_mac[5]);
    return true;
}

static void macro_pad_connection_callback(bool connected, void* context) {
    MacroPadApp* app = context;
    if(app) app->hid_connected = connected;
}

static bool macro_pad_usb_present(void) {
    return furi_hal_power_get_usb_voltage() >= MACRO_PAD_USB_PRESENT_VOLTS;
}

static bool macro_pad_transport_start_backend(
    MacroPadApp* app,
    MacroPadTransport transport) {
    furi_assert(app);
    if(app->hid_started || transport > MacroPadTransportUsb) return false;

    memset(&app->hid_config, 0, sizeof(app->hid_config));
    app->hid_connected = false;
    app->hid = bad_usb_hid_get_interface(
        transport == MacroPadTransportUsb ? BadUsbHidInterfaceUsb : BadUsbHidInterfaceBle);

    if(transport == MacroPadTransportUsb) {
        if(!bad_usb_hid_supports_usb() || furi_hal_usb_is_locked()) {
            FURI_LOG_E(TAG, "USB HID is unavailable or locked");
            app->hid = NULL;
            return false;
        }

        app->usb_previous = furi_hal_usb_get_config();
        furi_hal_usb_unlock();
        if(!furi_hal_usb_set_config(NULL, NULL)) {
            app->usb_previous = NULL;
            app->hid = NULL;
            return false;
        }

        strlcpy(app->hid_config.usb.manuf, "Momentum", sizeof(app->hid_config.usb.manuf));
        strlcpy(app->hid_config.usb.product, "Macro Pad", sizeof(app->hid_config.usb.product));
        app->hid->adjust_config(&app->hid_config);
        app->hid->init(&app->hid_config);

        if(furi_hal_usb_get_config() != &usb_hid) {
            furi_hal_usb_set_config(app->usb_previous, NULL);
            app->usb_previous = NULL;
            app->hid = NULL;
            return false;
        }

        app->hid_instance = NULL;
        app->hid_started = true;
        app->active_transport = MacroPadTransportUsb;
        app->active_transport_valid = true;
        app->hid->set_state_callback(NULL, macro_pad_connection_callback, app);
        app->hid_connected = app->hid->is_connected(NULL);
        FURI_LOG_I(TAG, "USB HID started");
        return true;
    }

    wlan_hal_stop_for_reconfigure();
    strlcpy(
        app->hid_config.ble.name,
        "Momentum Macro Pad",
        sizeof(app->hid_config.ble.name));
    app->hid_config.ble.bonding = true;
    app->hid_config.ble.pairing = GapPairingPinCodeVerifyYesNo;
    if(macro_pad_ble_identity_valid(app->ble_mac)) {
        memcpy(
            app->hid_config.ble.mac,
            app->ble_mac,
            sizeof(app->hid_config.ble.mac));
    }

    app->hid_instance = app->hid->init(&app->hid_config);
    if(!app->hid_instance) {
        FURI_LOG_E(TAG, "BLE HID failed to start");
        app->hid = NULL;
        return false;
    }

    app->hid_started = true;
    app->active_transport = MacroPadTransportBle;
    app->active_transport_valid = true;
    app->hid->set_state_callback(
        app->hid_instance, macro_pad_connection_callback, app);
    app->hid_connected = app->hid->is_connected(app->hid_instance);
    FURI_LOG_I(TAG, "BLE HID started as Momentum Macro Pad");
    return true;
}

static void macro_pad_transport_stop(MacroPadApp* app) {
    if(!app) return;
    if(!app->hid_started || !app->hid) {
        app->active_transport_valid = false;
        app->hid_connected = false;
        return;
    }

    app->hid->set_state_callback(app->hid_instance, NULL, NULL);
    app->hid->release_all(app->hid_instance);

    if(app->active_transport == MacroPadTransportUsb) {
        furi_hal_hid_set_state_callback(NULL, NULL);
        furi_hal_usb_set_config(NULL, NULL);
        furi_hal_usb_set_config(app->usb_previous, NULL);
        app->usb_previous = NULL;
    } else {
        app->hid->deinit(app->hid_instance);
    }

    app->hid_instance = NULL;
    app->hid = NULL;
    app->hid_started = false;
    app->hid_connected = false;
    app->active_transport_valid = false;
}

static bool macro_pad_transport_start(MacroPadApp* app) {
    app->transport_failed = false;
    app->auto_usb_deadline = 0;
    app->auto_usb_rejected = false;
    app->auto_vbus_present = macro_pad_usb_present();

    if(app->settings.transport == MacroPadTransportAuto) {
        if(app->auto_vbus_present &&
           macro_pad_transport_start_backend(app, MacroPadTransportUsb)) {
            app->auto_usb_deadline =
                furi_get_tick() + furi_ms_to_ticks(MACRO_PAD_AUTO_USB_SETTLE_MS);
            return true;
        }
        app->auto_usb_rejected = app->auto_vbus_present;
        return macro_pad_transport_start_backend(app, MacroPadTransportBle);
    }

    return macro_pad_transport_start_backend(app, app->settings.transport);
}

static void macro_pad_auto_transport_tick(MacroPadApp* app) {
    if(!app->in_run || app->settings.transport != MacroPadTransportAuto) return;

    const bool vbus_present = macro_pad_usb_present();
    const uint32_t now = furi_get_tick();
    if(!vbus_present) app->auto_usb_rejected = false;
    app->auto_vbus_present = vbus_present;

    if(!app->active_transport_valid) {
        const MacroPadTransport target =
            vbus_present && !app->auto_usb_rejected ? MacroPadTransportUsb :
                                                     MacroPadTransportBle;
        if(macro_pad_transport_start_backend(app, target)) {
            app->transport_failed = false;
            if(target == MacroPadTransportUsb) {
                app->auto_usb_deadline =
                    now + furi_ms_to_ticks(MACRO_PAD_AUTO_USB_SETTLE_MS);
            }
        }
        return;
    }

    if(app->active_transport == MacroPadTransportUsb) {
        if(app->hid_connected) {
            app->auto_usb_deadline = 0;
        } else {
            if(app->auto_usb_deadline == 0) {
                app->auto_usb_deadline =
                    now + furi_ms_to_ticks(MACRO_PAD_AUTO_USB_SETTLE_MS);
            }
            if((int32_t)(now - app->auto_usb_deadline) >= 0) {
                macro_pad_transport_stop(app);
                app->auto_usb_rejected = vbus_present;
                app->transport_failed =
                    !macro_pad_transport_start_backend(app, MacroPadTransportBle);
                app->auto_usb_deadline = 0;
            }
        }
    } else if(vbus_present && !app->auto_usb_rejected) {
        macro_pad_transport_stop(app);
        if(macro_pad_transport_start_backend(app, MacroPadTransportUsb)) {
            app->transport_failed = false;
            app->auto_usb_deadline =
                now + furi_ms_to_ticks(MACRO_PAD_AUTO_USB_SETTLE_MS);
        } else {
            app->auto_usb_rejected = true;
            app->transport_failed =
                !macro_pad_transport_start_backend(app, MacroPadTransportBle);
        }
    }
}

static void macro_pad_input_activity_callback(const void* value, void* context) {
    furi_assert(value);
    furi_assert(context);

    MacroPadApp* app = context;
    const InputEvent* event = value;
    if(app->wake_managed && event->type == InputTypePress) {
        app->wake_last_activity_tick = furi_get_tick();
        app->wake_rearm_requested = true;
    }
}

static void macro_pad_wake_start(MacroPadApp* app) {
    if(app->wake_managed) return;

    app->wake_last_activity_tick = furi_get_tick();
    app->wake_rearm_requested = false;
    app->wake_managed = true;

    if(!app->insomnia_held) {
        furi_hal_power_insomnia_enter();
        app->insomnia_held = true;
    }
    if(!app->wake_locked) {
        notification_message(app->notification, &sequence_display_backlight_enforce_on);
        app->wake_locked = true;
    }
}

static void macro_pad_wake_stop(MacroPadApp* app) {
    app->wake_managed = false;
    app->wake_rearm_requested = false;

    if(app->wake_locked) {
        notification_message(app->notification, &sequence_display_backlight_enforce_auto);
        app->wake_locked = false;
    }
    if(app->insomnia_held) {
        furi_hal_power_insomnia_exit();
        app->insomnia_held = false;
    }
}

static void macro_pad_wake_tick(MacroPadApp* app) {
    if(!app->wake_managed) return;

    if(app->wake_rearm_requested) {
        app->wake_rearm_requested = false;
        if(!app->wake_locked) {
            notification_message(app->notification, &sequence_display_backlight_enforce_on);
            app->wake_locked = true;
        }
    }
    if(app->settings.stay_awake) return;

    const uint32_t idle_ticks = furi_get_tick() - app->wake_last_activity_tick;
    if(app->wake_locked &&
       idle_ticks >= furi_ms_to_ticks(MACRO_PAD_DIM_TIMEOUT_MS)) {
        notification_message(app->notification, &sequence_display_backlight_enforce_auto);
        app->wake_locked = false;
        notification_display_dim(app->notification);
    }

    if(idle_ticks >= furi_ms_to_ticks(MACRO_PAD_SLEEP_TIMEOUT_MS)) {
        macro_pad_wake_stop(app);
        macro_pad_transport_stop(app);
        if(furi_record_exists(RECORD_DOLPHIN)) {
            Dolphin* dolphin = furi_record_open(RECORD_DOLPHIN);
            dolphin_prepare_for_sleep(dolphin);
            furi_record_close(RECORD_DOLPHIN);
        }
        furi_hal_rtc_arm_resume();
        furi_hal_power_shutdown();
    }
}

static void macro_pad_apply_profile_led(MacroPadApp* app) {
    const MacroPadLedColor color =
        app->settings.profiles[app->settings.active_profile].led_color;
    const NotificationSequence* sequence = &sequence_reset_rgb;
    switch(color) {
    case MacroPadLedRed:
        sequence = &sequence_set_only_red_255;
        break;
    case MacroPadLedGreen:
        sequence = &sequence_set_only_green_255;
        break;
    case MacroPadLedBlue:
        sequence = &sequence_set_only_blue_255;
        break;
    case MacroPadLedYellow:
        sequence = &sequence_solid_yellow;
        break;
    case MacroPadLedCyan:
        sequence = &macro_pad_led_cyan;
        break;
    case MacroPadLedMagenta:
        sequence = &macro_pad_led_magenta;
        break;
    case MacroPadLedWhite:
        sequence = &macro_pad_led_white;
        break;
    case MacroPadLedOff:
    default:
        break;
    }
    notification_message(app->notification, sequence);
}

static bool macro_pad_send_single_action(MacroPadApp* app, MacroPadAction action) {
    const MacroPadActionDefinition* definition = macro_pad_action_definition(action);
    if(definition->kind == MacroPadActionKindKeyboard) {
        app->hid->kb_press(app->hid_instance, definition->code);
        furi_delay_ms(MACRO_PAD_KEY_DELAY_MS);
        app->hid->kb_release(app->hid_instance, definition->code);
        return true;
    }
    if(definition->kind == MacroPadActionKindConsumer) {
        app->hid->consumer_press(app->hid_instance, definition->code);
        furi_delay_ms(MACRO_PAD_KEY_DELAY_MS);
        app->hid->consumer_release(app->hid_instance, definition->code);
        return true;
    }
    return false;
}

static bool macro_pad_sequence_has_later_action(
    const MacroPadSequence* sequence,
    uint8_t step) {
    for(uint8_t index = step + 1U; index < MACRO_PAD_SEQUENCE_STEP_COUNT; index++) {
        if(sequence->actions[index] != MacroPadActionNone) return true;
    }
    return false;
}

static void macro_pad_send_gesture(MacroPadApp* app, MacroPadGesture gesture) {
    if(!app->hid_started || !app->hid_connected || !app->hid ||
       gesture >= MACRO_PAD_GESTURE_COUNT) {
        return;
    }

    const MacroPadSequence* sequence =
        &app->settings.profiles[app->settings.active_profile].gestures[gesture];
    bool sent = false;
    for(uint8_t step = 0; step < MACRO_PAD_SEQUENCE_STEP_COUNT; step++) {
        sent |= macro_pad_send_single_action(app, sequence->actions[step]);
        if(sequence->actions[step] != MacroPadActionNone &&
           step < MACRO_PAD_SEQUENCE_STEP_COUNT - 1U &&
           macro_pad_sequence_has_later_action(sequence, step)) {
            furi_delay_ms(sequence->delays_ms[step]);
        }
    }

    if(sent) {
        app->last_action_gesture = gesture;
        app->last_action_tick = furi_get_tick();
    }
}

static uint8_t macro_pad_sequence_action_count(const MacroPadSequence* sequence) {
    uint8_t count = 0;
    for(uint8_t step = 0; step < MACRO_PAD_SEQUENCE_STEP_COUNT; step++) {
        count += sequence->actions[step] != MacroPadActionNone;
    }
    return count;
}

static void macro_pad_sequence_summary(
    const MacroPadSequence* sequence,
    char* buffer,
    size_t size) {
    uint8_t count = 0;
    MacroPadAction first = MacroPadActionNone;
    for(uint8_t step = 0; step < MACRO_PAD_SEQUENCE_STEP_COUNT; step++) {
        if(sequence->actions[step] != MacroPadActionNone) {
            if(count == 0) first = sequence->actions[step];
            count++;
        }
    }

    if(count == 0) {
        strlcpy(buffer, "None", size);
    } else if(count == 1) {
        strlcpy(buffer, macro_pad_action_definition(first)->short_label, size);
    } else {
        snprintf(
            buffer,
            size,
            "%s +%u",
            macro_pad_action_definition(first)->short_label,
            count - 1U);
    }
}

static uint8_t macro_pad_category_action_count(MacroPadActionCategory category) {
    switch(category) {
    case MacroPadActionCategoryEditing:
        return COUNT_OF(macro_pad_editing_actions);
    case MacroPadActionCategoryMedia:
        return COUNT_OF(macro_pad_media_actions);
    case MacroPadActionCategoryNavigation:
        return COUNT_OF(macro_pad_navigation_actions);
    case MacroPadActionCategoryMac:
        return COUNT_OF(macro_pad_mac_actions);
    case MacroPadActionCategoryAll:
    default:
        return MacroPadActionCount;
    }
}

static MacroPadAction macro_pad_category_action(
    MacroPadActionCategory category,
    uint8_t index) {
    switch(category) {
    case MacroPadActionCategoryEditing:
        return macro_pad_editing_actions[index];
    case MacroPadActionCategoryMedia:
        return macro_pad_media_actions[index];
    case MacroPadActionCategoryNavigation:
        return macro_pad_navigation_actions[index];
    case MacroPadActionCategoryMac:
        return macro_pad_mac_actions[index];
    case MacroPadActionCategoryAll:
    default:
        return index;
    }
}

static uint8_t macro_pad_category_action_index(
    MacroPadActionCategory category,
    MacroPadAction action) {
    const uint8_t count = macro_pad_category_action_count(category);
    for(uint8_t index = 0; index < count; index++) {
        if(macro_pad_category_action(category, index) == action) return index;
    }
    return 0;
}

static MacroPadActionCategory macro_pad_action_category(MacroPadAction action) {
    if(action >= MacroPadActionPlayPause && action <= MacroPadActionPreviousTrack) {
        return MacroPadActionCategoryMedia;
    }
    if(action >= MacroPadActionMacCopy && action <= MacroPadActionMacFind) {
        return MacroPadActionCategoryMac;
    }
    if(action >= MacroPadActionPageUp && action <= MacroPadActionBrowserForward) {
        return MacroPadActionCategoryNavigation;
    }
    return MacroPadActionCategoryEditing;
}

static MacroPadAction macro_pad_category_default_action(MacroPadActionCategory category) {
    switch(category) {
    case MacroPadActionCategoryMedia:
        return MacroPadActionPlayPause;
    case MacroPadActionCategoryNavigation:
        return MacroPadActionEnter;
    case MacroPadActionCategoryMac:
        return MacroPadActionMacCopy;
    case MacroPadActionCategoryAll:
        return MacroPadActionNone;
    case MacroPadActionCategoryEditing:
    default:
        return MacroPadActionCopy;
    }
}

static void macro_pad_set_simple_action(MacroPadApp* app, MacroPadAction action) {
    MacroPadSequence* sequence =
        &app->settings.profiles[app->settings.active_profile].gestures[app->edit_gesture];
    memset(sequence->actions, MacroPadActionNone, sizeof(sequence->actions));
    sequence->actions[0] = action;
    app->settings_dirty = true;
}

static void macro_pad_draw_highlighted_text(
    Canvas* canvas,
    uint8_t x,
    uint8_t y,
    uint8_t width,
    bool active,
    const char* text) {
    if(active) {
        canvas_draw_rbox(canvas, x, y - 9, width, 10, 2);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_str(canvas, x + 2, y, text);
    if(active) canvas_set_color(canvas, ColorBlack);
}

static void macro_pad_draw_callback(Canvas* canvas, void* context) {
    MacroPadViewModel* model = context;
    char line[48];
    char action[20];

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    snprintf(
        line,
        sizeof(line),
        "Macro Pad | %s",
        macro_pad_profile_label(model->profile));
    canvas_draw_str(canvas, 2, 10, line);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);
    if(model->start_failed || !model->active_transport_valid) {
        snprintf(
            line,
            sizeof(line),
            "%s unavailable",
            macro_pad_transport_label(model->configured_transport));
    } else {
        const char* transport =
            model->active_transport == MacroPadTransportUsb ? "USB" : "BLE";
        if(model->connected) {
            snprintf(
                line,
                sizeof(line),
                model->configured_transport == MacroPadTransportAuto ?
                    "AUTO>%s connected" :
                    "%s connected",
                transport);
        } else {
            const uint8_t dots = 1U + (model->reconnect_phase % 3U);
            snprintf(
                line,
                sizeof(line),
                model->active_transport == MacroPadTransportUsb ?
                    "USB waiting%.*s" :
                    "BLE reconnecting%.*s",
                dots,
                "...");
        }
    }
    canvas_draw_str_aligned(canvas, 64, 21, AlignCenter, AlignBottom, line);

    snprintf(
        action,
        sizeof(action),
        "CCW %s",
        macro_pad_action_definition(model->first_actions[MacroPadGestureCcw])->short_label);
    if(model->action_counts[MacroPadGestureCcw] > 1U) {
        strlcat(action, "+", sizeof(action));
    }
    macro_pad_draw_highlighted_text(
        canvas,
        0,
        31,
        64,
        model->active_gesture == MacroPadGestureCcw,
        action);

    snprintf(
        action,
        sizeof(action),
        "CW %s",
        macro_pad_action_definition(model->first_actions[MacroPadGestureCw])->short_label);
    if(model->action_counts[MacroPadGestureCw] > 1U) {
        strlcat(action, "+", sizeof(action));
    }
    macro_pad_draw_highlighted_text(
        canvas,
        64,
        31,
        64,
        model->active_gesture == MacroPadGestureCw,
        action);

    static const char* const labels[] = {"Click", "2x", "Hold"};
    for(uint8_t row = 0; row < 3U; row++) {
        const uint8_t gesture = MacroPadGesturePress + row;
        snprintf(
            action,
            sizeof(action),
            "%s %s%s",
            labels[row],
            macro_pad_action_definition(model->first_actions[gesture])->short_label,
            model->action_counts[gesture] > 1U ? "+" : "");
        macro_pad_draw_highlighted_text(
            canvas,
            0,
            41U + row * 10U,
            128,
            model->active_gesture == gesture,
            action);
    }
}

static bool macro_pad_run_input_callback(InputEvent* event, void* context) {
    MacroPadApp* app = context;
    uint32_t custom_event = 0;

    if(event->key == InputKeyOk && event->type == InputTypeLong) {
        app->pending_press = false;
        custom_event = MacroPadEventLongPress;
    } else if(event->type == InputTypeShort) {
        switch(event->key) {
        case InputKeyLeft:
            custom_event = MacroPadEventCcw;
            break;
        case InputKeyRight:
            custom_event = MacroPadEventCw;
            break;
        case InputKeyDown:
            custom_event = MacroPadEventProfilePrevious;
            break;
        case InputKeyUp:
            custom_event = MacroPadEventProfileNext;
            break;
        case InputKeyOk:
            if(app->pending_press &&
               furi_get_tick() - app->pending_press_tick <=
                   furi_ms_to_ticks(MACRO_PAD_DOUBLE_PRESS_MS)) {
                app->pending_press = false;
                custom_event = MacroPadEventDoublePress;
            } else {
                app->pending_press = true;
                app->pending_press_tick = furi_get_tick();
                return true;
            }
            break;
        default:
            return false;
        }
    } else {
        return false;
    }

    view_dispatcher_send_custom_event(app->view_dispatcher, custom_event);
    return true;
}

static void macro_pad_refresh_run_view(MacroPadApp* app) {
    if(!app->in_run) return;

    MacroPadViewModel* model = view_get_model(app->run_view);
    model->configured_transport = app->settings.transport;
    model->active_transport = app->active_transport;
    model->active_transport_valid = app->active_transport_valid;
    model->profile = app->settings.active_profile;
    model->connected = app->hid_connected;
    model->start_failed = app->transport_failed;
    model->stay_awake = app->settings.stay_awake;
    model->reconnect_phase = (furi_get_tick() / furi_ms_to_ticks(250U)) & 0x03U;

    const MacroPadProfile* profile =
        &app->settings.profiles[app->settings.active_profile];
    for(uint8_t gesture = 0; gesture < MACRO_PAD_GESTURE_COUNT; gesture++) {
        const MacroPadSequence* sequence = &profile->gestures[gesture];
        model->first_actions[gesture] = MacroPadActionNone;
        for(uint8_t step = 0; step < MACRO_PAD_SEQUENCE_STEP_COUNT; step++) {
            if(sequence->actions[step] != MacroPadActionNone) {
                model->first_actions[gesture] = sequence->actions[step];
                break;
            }
        }
        model->action_counts[gesture] = macro_pad_sequence_action_count(sequence);
    }

    model->active_gesture =
        app->last_action_gesture < MACRO_PAD_GESTURE_COUNT &&
                furi_get_tick() - app->last_action_tick <
                    furi_ms_to_ticks(MACRO_PAD_FEEDBACK_MS) ?
            app->last_action_gesture :
            MACRO_PAD_GESTURE_COUNT;
    view_commit_model(app->run_view, true);
}

static void macro_pad_update_config_items(MacroPadApp* app) {
    VariableItem* item = variable_item_list_get(app->config_list, MacroPadRowTransport);
    variable_item_set_current_value_index(item, app->settings.transport);
    variable_item_set_current_value_text(
        item, macro_pad_transport_label(app->settings.transport));

    item = variable_item_list_get(app->config_list, MacroPadRowProfile);
    variable_item_set_current_value_index(item, app->settings.active_profile);
    variable_item_set_current_value_text(
        item, macro_pad_profile_label(app->settings.active_profile));

    MacroPadProfile* profile =
        &app->settings.profiles[app->settings.active_profile];
    item = variable_item_list_get(app->config_list, MacroPadRowLed);
    variable_item_set_current_value_index(item, profile->led_color);
    variable_item_set_current_value_text(item, macro_pad_led_label(profile->led_color));

    item = variable_item_list_get(app->config_list, MacroPadRowStayAwake);
    variable_item_set_current_value_index(item, app->settings.stay_awake ? 1U : 0U);
    variable_item_set_current_value_text(
        item, app->settings.stay_awake ? "On" : "Auto sleep");

    for(uint8_t gesture = 0; gesture < MACRO_PAD_GESTURE_COUNT; gesture++) {
        char summary[24];
        macro_pad_sequence_summary(&profile->gestures[gesture], summary, sizeof(summary));
        item = variable_item_list_get(app->config_list, MacroPadRowCcw + gesture);
        variable_item_set_current_value_text(item, summary);
    }

    item = variable_item_list_get(app->config_list, MacroPadRowCopyProfile);
    variable_item_set_current_value_text(item, "Copy current");
    item = variable_item_list_get(app->config_list, MacroPadRowPasteProfile);
    variable_item_set_current_value_text(item, "Paste here");
    variable_item_set_locked(
        item, !app->profile_clipboard_valid, "Copy a profile first");
}

static void macro_pad_transport_changed(VariableItem* item) {
    MacroPadApp* app = variable_item_get_context(item);
    app->settings.transport = variable_item_get_current_value_index(item);
    app->settings_dirty = true;
    variable_item_set_current_value_text(
        item, macro_pad_transport_label(app->settings.transport));
}

static void macro_pad_profile_changed(VariableItem* item) {
    MacroPadApp* app = variable_item_get_context(item);
    app->settings.active_profile = variable_item_get_current_value_index(item);
    app->settings_dirty = true;
    macro_pad_update_config_items(app);
}

static void macro_pad_led_changed(VariableItem* item) {
    MacroPadApp* app = variable_item_get_context(item);
    MacroPadProfile* profile =
        &app->settings.profiles[app->settings.active_profile];
    profile->led_color = variable_item_get_current_value_index(item);
    app->settings_dirty = true;
    variable_item_set_current_value_text(item, macro_pad_led_label(profile->led_color));
}

static void macro_pad_stay_awake_changed(VariableItem* item) {
    MacroPadApp* app = variable_item_get_context(item);
    app->settings.stay_awake = variable_item_get_current_value_index(item) != 0;
    app->settings_dirty = true;
    variable_item_set_current_value_text(
        item, app->settings.stay_awake ? "On" : "Auto sleep");
}

static void macro_pad_config_enter_callback(void* context, uint32_t index) {
    MacroPadApp* app = context;
    if(index == MacroPadRowRun) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MacroPadEventRun);
    } else if(index >= MacroPadRowCcw && index <= MacroPadRowLongPress) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, MacroPadEventEditCcw + index - MacroPadRowCcw);
    } else if(index == MacroPadRowCopyProfile) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MacroPadEventProfileCopy);
    } else if(index == MacroPadRowPasteProfile) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, MacroPadEventProfilePasteOpen);
    } else if(index == MacroPadRowExport) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MacroPadEventExport);
    } else if(index == MacroPadRowImport) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MacroPadEventImportOpen);
    } else if(index == MacroPadRowRepair) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MacroPadEventRepairOpen);
    } else if(index == MacroPadRowResetProfile) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MacroPadEventResetProfile);
    }
}

static void macro_pad_build_config(MacroPadApp* app) {
    variable_item_list_set_header(app->config_list, "Macro Pad");

    VariableItem* item = variable_item_list_add(app->config_list, "Run", 1, NULL, NULL);
    variable_item_set_current_value_text(item, "Start");

    variable_item_list_add(
        app->config_list,
        "Transport",
        MacroPadTransportCount,
        macro_pad_transport_changed,
        app);
    variable_item_list_add(
        app->config_list,
        "Profile",
        MACRO_PAD_PROFILE_COUNT,
        macro_pad_profile_changed,
        app);
    variable_item_list_add(
        app->config_list,
        "Profile LED",
        MacroPadLedCount,
        macro_pad_led_changed,
        app);
    variable_item_list_add(
        app->config_list,
        "Stay Awake",
        2,
        macro_pad_stay_awake_changed,
        app);

    for(uint8_t gesture = 0; gesture < MACRO_PAD_GESTURE_COUNT; gesture++) {
        variable_item_list_add(
            app->config_list, macro_pad_gesture_label(gesture), 1, NULL, NULL);
    }

    item = variable_item_list_add(app->config_list, "Copy Profile", 1, NULL, NULL);
    variable_item_set_current_value_text(item, "Copy current");
    item = variable_item_list_add(app->config_list, "Paste Profile", 1, NULL, NULL);
    variable_item_set_current_value_text(item, "Paste here");

    item = variable_item_list_add(app->config_list, "Export Profiles", 1, NULL, NULL);
    variable_item_set_current_value_text(item, "Write SD");
    item = variable_item_list_add(app->config_list, "Import Profiles", 1, NULL, NULL);
    variable_item_set_current_value_text(item, "Read SD");
    item = variable_item_list_add(app->config_list, "Repair BLE", 1, NULL, NULL);
    variable_item_set_current_value_text(item, "Renew ID");
    item = variable_item_list_add(app->config_list, "Reset Profile", 1, NULL, NULL);
    variable_item_set_current_value_text(item, "Defaults");

    variable_item_list_set_enter_callback(
        app->config_list, macro_pad_config_enter_callback, app);
    macro_pad_update_config_items(app);
}

static MacroPadAction macro_pad_sequence_first_action(const MacroPadSequence* sequence) {
    for(uint8_t step = 0; step < MACRO_PAD_SEQUENCE_STEP_COUNT; step++) {
        if(sequence->actions[step] != MacroPadActionNone) return sequence->actions[step];
    }
    return MacroPadActionNone;
}

static void macro_pad_gesture_category_changed(VariableItem* item) {
    MacroPadApp* app = variable_item_get_context(item);
    app->edit_category = variable_item_get_current_value_index(item);
    macro_pad_set_simple_action(
        app, macro_pad_category_default_action(app->edit_category));
    macro_pad_update_gesture_items(app);
}

static void macro_pad_gesture_action_changed(VariableItem* item) {
    MacroPadApp* app = variable_item_get_context(item);
    const uint8_t value_index = variable_item_get_current_value_index(item);
    const MacroPadAction action =
        macro_pad_category_action(app->edit_category, value_index);
    macro_pad_set_simple_action(app, action);
    variable_item_set_current_value_text(item, macro_pad_action_definition(action)->label);
}

static void macro_pad_gesture_enter_callback(void* context, uint32_t index) {
    MacroPadApp* app = context;
    switch(index) {
    case MacroPadGestureRowTest:
        view_dispatcher_send_custom_event(app->view_dispatcher, MacroPadEventGestureTest);
        break;
    case MacroPadGestureRowAdvanced:
        view_dispatcher_send_custom_event(app->view_dispatcher, MacroPadEventGestureAdvanced);
        break;
    case MacroPadGestureRowCopy:
        view_dispatcher_send_custom_event(app->view_dispatcher, MacroPadEventGestureCopy);
        break;
    case MacroPadGestureRowPaste:
        view_dispatcher_send_custom_event(app->view_dispatcher, MacroPadEventGesturePaste);
        break;
    default:
        break;
    }
}

static void macro_pad_update_gesture_items(MacroPadApp* app) {
    char header[32];
    snprintf(
        header,
        sizeof(header),
        "%s Simple | %s",
        macro_pad_gesture_short_labels[app->edit_gesture],
        macro_pad_profile_label(app->settings.active_profile));
    variable_item_list_set_header(app->gesture_list, header);

    MacroPadSequence* sequence = macro_pad_edit_sequence(app);
    const MacroPadAction action = macro_pad_sequence_first_action(sequence);

    VariableItem* item =
        variable_item_list_get(app->gesture_list, MacroPadGestureRowCategory);
    variable_item_set_current_value_index(item, app->edit_category);
    variable_item_set_current_value_text(
        item, macro_pad_category_labels[app->edit_category]);

    item = variable_item_list_get(app->gesture_list, MacroPadGestureRowAction);
    variable_item_set_values_count(
        item, macro_pad_category_action_count(app->edit_category));
    variable_item_set_current_value_index(
        item, macro_pad_category_action_index(app->edit_category, action));
    variable_item_set_current_value_text(item, macro_pad_action_definition(action)->label);

    item = variable_item_list_get(app->gesture_list, MacroPadGestureRowTest);
    variable_item_set_current_value_text(item, "Run once");
    item = variable_item_list_get(app->gesture_list, MacroPadGestureRowAdvanced);
    char summary[24];
    macro_pad_sequence_summary(sequence, summary, sizeof(summary));
    variable_item_set_current_value_text(item, summary);
    item = variable_item_list_get(app->gesture_list, MacroPadGestureRowCopy);
    variable_item_set_current_value_text(item, "Copy");
    item = variable_item_list_get(app->gesture_list, MacroPadGestureRowPaste);
    variable_item_set_current_value_text(item, "Paste");
    variable_item_set_locked(
        item, !app->gesture_clipboard_valid, "Copy a gesture first");
}

static void macro_pad_build_gesture_editor(MacroPadApp* app) {
    variable_item_list_add(
        app->gesture_list,
        "Category",
        MacroPadActionCategoryCount,
        macro_pad_gesture_category_changed,
        app);
    variable_item_list_add(
        app->gesture_list,
        "Action",
        MacroPadActionCount,
        macro_pad_gesture_action_changed,
        app);

    VariableItem* item =
        variable_item_list_add(app->gesture_list, "Test Macro", 1, NULL, NULL);
    variable_item_set_current_value_text(item, "Run once");
    item = variable_item_list_add(app->gesture_list, "Advanced", 1, NULL, NULL);
    variable_item_set_current_value_text(item, "4-step editor");
    item = variable_item_list_add(app->gesture_list, "Copy Gesture", 1, NULL, NULL);
    variable_item_set_current_value_text(item, "Copy");
    item = variable_item_list_add(app->gesture_list, "Paste Gesture", 1, NULL, NULL);
    variable_item_set_current_value_text(item, "Paste");
    variable_item_list_set_enter_callback(
        app->gesture_list, macro_pad_gesture_enter_callback, app);
}

static uint8_t macro_pad_delay_index(uint16_t delay_ms) {
    uint8_t closest = 0;
    uint16_t closest_delta = UINT16_MAX;
    for(uint8_t index = 0; index < COUNT_OF(macro_pad_delay_values); index++) {
        const uint16_t value = macro_pad_delay_values[index];
        const uint16_t delta = value > delay_ms ? value - delay_ms : delay_ms - value;
        if(delta < closest_delta) {
            closest = index;
            closest_delta = delta;
        }
    }
    return closest;
}

static MacroPadSequence* macro_pad_edit_sequence(MacroPadApp* app) {
    return &app->settings.profiles[app->settings.active_profile]
                .gestures[app->edit_gesture];
}

static void macro_pad_sequence_item_changed(VariableItem* item) {
    MacroPadApp* app = variable_item_get_context(item);
    const uint8_t row = variable_item_list_get_selected_item_index(app->sequence_list);
    MacroPadSequence* sequence = macro_pad_edit_sequence(app);

    if((row & 1U) == 0) {
        const uint8_t step = row / 2U;
        const MacroPadAction action = variable_item_get_current_value_index(item);
        sequence->actions[step] = action;
        variable_item_set_current_value_text(item, macro_pad_action_definition(action)->label);
    } else {
        const uint8_t delay = row / 2U;
        const uint8_t value_index = variable_item_get_current_value_index(item);
        sequence->delays_ms[delay] = macro_pad_delay_values[value_index];
        variable_item_set_current_value_text(item, macro_pad_delay_labels[value_index]);
    }
    app->settings_dirty = true;
}

static void macro_pad_update_sequence_items(MacroPadApp* app) {
    variable_item_list_set_header(
        app->sequence_list, macro_pad_editor_headers[app->edit_gesture]);

    MacroPadSequence* sequence = macro_pad_edit_sequence(app);
    for(uint8_t row = 0; row < MacroPadSequenceRowCount; row++) {
        VariableItem* item = variable_item_list_get(app->sequence_list, row);
        if((row & 1U) == 0) {
            const uint8_t step = row / 2U;
            const MacroPadAction action = sequence->actions[step];
            variable_item_set_current_value_index(item, action);
            variable_item_set_current_value_text(
                item, macro_pad_action_definition(action)->label);
        } else {
            const uint8_t delay = row / 2U;
            const uint8_t value_index = macro_pad_delay_index(sequence->delays_ms[delay]);
            sequence->delays_ms[delay] = macro_pad_delay_values[value_index];
            variable_item_set_current_value_index(item, value_index);
            variable_item_set_current_value_text(item, macro_pad_delay_labels[value_index]);
        }
    }
}

static void macro_pad_build_sequence_editor(MacroPadApp* app) {
    for(uint8_t row = 0; row < MacroPadSequenceRowCount; row++) {
        if((row & 1U) == 0) {
            variable_item_list_add(
                app->sequence_list,
                macro_pad_sequence_row_labels[row],
                MacroPadActionCount,
                macro_pad_sequence_item_changed,
                app);
        } else {
            variable_item_list_add(
                app->sequence_list,
                macro_pad_sequence_row_labels[row],
                COUNT_OF(macro_pad_delay_values),
                macro_pad_sequence_item_changed,
                app);
        }
    }
}

static void macro_pad_dialog_callback(DialogExResult result, void* context) {
    MacroPadApp* app = context;
    if(result == DialogExResultRight &&
       app->dialog_mode == MacroPadDialogRepairConfirm) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, MacroPadEventRepairConfirm);
    } else if(
        result == DialogExResultRight &&
        app->dialog_mode == MacroPadDialogImportConfirm) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, MacroPadEventImportConfirm);
    } else if(
        result == DialogExResultRight &&
        app->dialog_mode == MacroPadDialogProfilePasteConfirm) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, MacroPadEventProfilePasteConfirm);
    } else if(result == DialogExResultLeft || result == DialogExResultCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MacroPadEventDialogClose);
    }
}

static void macro_pad_show_repair_confirm(MacroPadApp* app) {
    app->dialog_mode = MacroPadDialogRepairConfirm;
    dialog_ex_reset(app->dialog);
    dialog_ex_set_header(app->dialog, "Repair BLE pairing?", 64, 8, AlignCenter, AlignCenter);
    dialog_ex_set_text(
        app->dialog,
        "Renews Macro Pad ID\nand stops stale reconnects",
        64,
        31,
        AlignCenter,
        AlignCenter);
    dialog_ex_set_left_button_text(app->dialog, "Cancel");
    dialog_ex_set_right_button_text(app->dialog, "Repair");
}

static void macro_pad_show_import_confirm(MacroPadApp* app) {
    app->dialog_mode = MacroPadDialogImportConfirm;
    dialog_ex_reset(app->dialog);
    dialog_ex_set_header(app->dialog, "Import profiles?", 64, 8, AlignCenter, AlignCenter);
    dialog_ex_set_text(
        app->dialog,
        "Replace current setup from\nmacro_pad_export.macro",
        64,
        31,
        AlignCenter,
        AlignCenter);
    dialog_ex_set_left_button_text(app->dialog, "Cancel");
    dialog_ex_set_right_button_text(app->dialog, "Import");
}

static void macro_pad_show_profile_paste_confirm(MacroPadApp* app) {
    char text[64];
    app->dialog_mode = MacroPadDialogProfilePasteConfirm;
    dialog_ex_reset(app->dialog);
    dialog_ex_set_header(app->dialog, "Paste profile?", 64, 8, AlignCenter, AlignCenter);
    snprintf(
        text,
        sizeof(text),
        "Replace %s with\ncopied %s settings",
        macro_pad_profile_label(app->settings.active_profile),
        macro_pad_profile_label(app->profile_clipboard_source));
    dialog_ex_set_text(app->dialog, text, 64, 31, AlignCenter, AlignCenter);
    dialog_ex_set_left_button_text(app->dialog, "Cancel");
    dialog_ex_set_right_button_text(app->dialog, "Paste");
}

static void macro_pad_show_result(
    MacroPadApp* app,
    const char* header,
    const char* text,
    bool repair_result) {
    app->dialog_mode =
        repair_result ? MacroPadDialogRepairResult : MacroPadDialogTransferResult;
    dialog_ex_reset(app->dialog);
    dialog_ex_set_header(app->dialog, header, 64, 8, AlignCenter, AlignCenter);
    dialog_ex_set_text(app->dialog, text, 64, 31, AlignCenter, AlignCenter);
    dialog_ex_set_center_button_text(app->dialog, "OK");
}

static void macro_pad_open_dialog(MacroPadApp* app) {
    app->in_dialog = true;
    view_dispatcher_switch_to_view(app->view_dispatcher, MacroPadViewDialog);
}

static void macro_pad_cycle_profile(MacroPadApp* app, int8_t direction) {
    int8_t profile = (int8_t)app->settings.active_profile + direction;
    if(profile < 0) profile = MACRO_PAD_PROFILE_COUNT - 1U;
    if(profile >= MACRO_PAD_PROFILE_COUNT) profile = 0;
    app->settings.active_profile = profile;
    app->settings_dirty = true;
    app->pending_press = false;
    app->last_action_gesture = MACRO_PAD_GESTURE_COUNT;
    macro_pad_apply_profile_led(app);
    macro_pad_update_config_items(app);
    macro_pad_refresh_run_view(app);
}

static void macro_pad_enter_run(MacroPadApp* app) {
    if(app->settings_dirty && macro_pad_settings_save(&app->settings)) {
        app->settings_dirty = false;
    }
    app->in_run = true;
    app->pending_press = false;
    app->last_action_gesture = MACRO_PAD_GESTURE_COUNT;
    app->transport_failed = !macro_pad_transport_start(app);
    macro_pad_wake_start(app);
    macro_pad_apply_profile_led(app);
    macro_pad_refresh_run_view(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, MacroPadViewRun);
}

static bool macro_pad_custom_event_callback(void* context, uint32_t event) {
    MacroPadApp* app = context;

    if(event == MacroPadEventRun && !app->in_run) {
        app->test_gesture_pending = false;
        app->test_return_to_gesture = false;
        macro_pad_enter_run(app);
        return true;
    }

    if(event == MacroPadEventResetProfile && !app->in_run) {
        macro_pad_settings_reset_profile(&app->settings, app->settings.active_profile);
        app->settings_dirty = true;
        macro_pad_update_config_items(app);
        notification_message(app->notification, &sequence_blink_yellow_100);
        return true;
    }

    if(event == MacroPadEventExport && !app->in_run) {
        const bool success = macro_pad_settings_export(&app->settings);
        macro_pad_show_result(
            app,
            success ? "Profiles exported" : "Export failed",
            success ? "/ext/macro/\nmacro_pad_export.macro" :
                      "Check the SD card\nand try again",
            false);
        notification_message(
            app->notification,
            success ? &sequence_blink_green_100 : &sequence_blink_red_100);
        macro_pad_open_dialog(app);
        return true;
    }

    if(event == MacroPadEventImportOpen && !app->in_run && !app->in_dialog) {
        macro_pad_show_import_confirm(app);
        macro_pad_open_dialog(app);
        return true;
    }

    if(event == MacroPadEventImportConfirm && app->in_dialog) {
        const bool success = macro_pad_settings_import(&app->settings);
        if(success) {
            app->settings_dirty = !macro_pad_settings_save(&app->settings);
            macro_pad_update_config_items(app);
        }
        macro_pad_show_result(
            app,
            success ? "Profiles imported" : "Import failed",
            success ? "Profiles and macros\nare ready" :
                      "Export file missing\nor invalid",
            false);
        notification_message(
            app->notification,
            success ? &sequence_blink_green_100 : &sequence_blink_red_100);
        return true;
    }

    if(event == MacroPadEventRepairOpen && !app->in_run && !app->in_dialog) {
        macro_pad_show_repair_confirm(app);
        macro_pad_open_dialog(app);
        return true;
    }

    if(event == MacroPadEventRepairConfirm && app->in_dialog) {
        const bool success = macro_pad_ble_identity_renew(app);
        if(success) {
            app->settings.transport = MacroPadTransportBle;
            app->settings_dirty = true;
            macro_pad_update_config_items(app);
        }
        macro_pad_show_result(
            app,
            success ? "BLE repaired" : "Repair failed",
            success ? "Run, then pair with\nMomentum Macro Pad" :
                      "Identity was not saved\nTry again",
            true);
        notification_message(
            app->notification,
            success ? &sequence_blink_green_100 : &sequence_blink_red_100);
        return true;
    }

    if(event == MacroPadEventProfileCopy && !app->in_run) {
        app->profile_clipboard =
            app->settings.profiles[app->settings.active_profile];
        app->profile_clipboard_source = app->settings.active_profile;
        app->profile_clipboard_valid = true;
        macro_pad_update_config_items(app);
        notification_message(app->notification, &sequence_blink_green_100);
        return true;
    }

    if(event == MacroPadEventProfilePasteOpen && !app->in_run &&
       app->profile_clipboard_valid) {
        macro_pad_show_profile_paste_confirm(app);
        macro_pad_open_dialog(app);
        return true;
    }

    if(event == MacroPadEventProfilePasteConfirm && app->in_dialog &&
       app->profile_clipboard_valid) {
        app->settings.profiles[app->settings.active_profile] = app->profile_clipboard;
        app->settings_dirty = true;
        macro_pad_update_config_items(app);
        macro_pad_show_result(
            app, "Profile pasted", "Macros and LED color\nwere copied", false);
        notification_message(app->notification, &sequence_blink_green_100);
        return true;
    }

    if(event == MacroPadEventDialogClose && app->in_dialog) {
        app->in_dialog = false;
        app->dialog_mode = MacroPadDialogNone;
        dialog_ex_reset(app->dialog);
        view_dispatcher_switch_to_view(app->view_dispatcher, MacroPadViewConfig);
        return true;
    }

    if(event >= MacroPadEventEditCcw && event <= MacroPadEventEditLongPress &&
       !app->in_run && !app->in_dialog) {
        app->edit_gesture = event - MacroPadEventEditCcw;
        const MacroPadAction action =
            macro_pad_sequence_first_action(macro_pad_edit_sequence(app));
        app->edit_category = macro_pad_action_category(action);
        app->in_gesture = true;
        macro_pad_update_gesture_items(app);
        variable_item_list_set_selected_item(
            app->gesture_list, MacroPadGestureRowAction);
        view_dispatcher_switch_to_view(app->view_dispatcher, MacroPadViewGesture);
        return true;
    }

    if(event == MacroPadEventGestureAdvanced && app->in_gesture) {
        app->in_gesture = false;
        app->in_sequence = true;
        macro_pad_update_sequence_items(app);
        variable_item_list_set_selected_item(app->sequence_list, MacroPadSequenceRowStep1);
        view_dispatcher_switch_to_view(app->view_dispatcher, MacroPadViewSequence);
        return true;
    }

    if(event == MacroPadEventGestureCopy && app->in_gesture) {
        app->gesture_clipboard = *macro_pad_edit_sequence(app);
        app->gesture_clipboard_valid = true;
        macro_pad_update_gesture_items(app);
        notification_message(app->notification, &sequence_blink_green_100);
        return true;
    }

    if(event == MacroPadEventGesturePaste && app->in_gesture &&
       app->gesture_clipboard_valid) {
        *macro_pad_edit_sequence(app) = app->gesture_clipboard;
        app->settings_dirty = true;
        app->edit_category =
            macro_pad_action_category(macro_pad_sequence_first_action(macro_pad_edit_sequence(app)));
        macro_pad_update_gesture_items(app);
        notification_message(app->notification, &sequence_blink_green_100);
        return true;
    }

    if(event == MacroPadEventGestureTest && app->in_gesture) {
        app->in_gesture = false;
        app->test_gesture_pending = true;
        app->test_return_to_gesture = true;
        macro_pad_enter_run(app);
        return true;
    }

    if(app->in_run && event >= MacroPadEventCcw && event <= MacroPadEventLongPress) {
        macro_pad_send_gesture(app, event - MacroPadEventCcw);
        macro_pad_refresh_run_view(app);
        return true;
    }

    if(app->in_run && event == MacroPadEventProfilePrevious) {
        macro_pad_cycle_profile(app, -1);
        return true;
    }

    if(app->in_run && event == MacroPadEventProfileNext) {
        macro_pad_cycle_profile(app, 1);
        return true;
    }

    return false;
}

static bool macro_pad_back_event_callback(void* context) {
    MacroPadApp* app = context;
    if(app->in_dialog) {
        app->in_dialog = false;
        app->dialog_mode = MacroPadDialogNone;
        dialog_ex_reset(app->dialog);
        view_dispatcher_switch_to_view(app->view_dispatcher, MacroPadViewConfig);
        return true;
    }

    if(app->in_sequence) {
        app->in_sequence = false;
        app->in_gesture = true;
        app->edit_category =
            macro_pad_action_category(macro_pad_sequence_first_action(macro_pad_edit_sequence(app)));
        macro_pad_update_gesture_items(app);
        variable_item_list_set_selected_item(
            app->gesture_list, MacroPadGestureRowAdvanced);
        view_dispatcher_switch_to_view(app->view_dispatcher, MacroPadViewGesture);
        return true;
    }

    if(app->in_run) {
        app->pending_press = false;
        notification_message(app->notification, &sequence_reset_rgb);
        macro_pad_wake_stop(app);
        macro_pad_transport_stop(app);
        app->transport_failed = false;
        app->in_run = false;
        app->test_gesture_pending = false;
        if(app->test_return_to_gesture) {
            app->test_return_to_gesture = false;
            app->in_gesture = true;
            macro_pad_update_gesture_items(app);
            variable_item_list_set_selected_item(
                app->gesture_list, MacroPadGestureRowTest);
            view_dispatcher_switch_to_view(app->view_dispatcher, MacroPadViewGesture);
        } else {
            view_dispatcher_switch_to_view(app->view_dispatcher, MacroPadViewConfig);
        }
        return true;
    }

    if(app->in_gesture) {
        app->in_gesture = false;
        macro_pad_update_config_items(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, MacroPadViewConfig);
        return true;
    }

    if(app->settings_dirty) {
        macro_pad_settings_save(&app->settings);
        app->settings_dirty = false;
    }
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

static void macro_pad_tick_event_callback(void* context) {
    MacroPadApp* app = context;
    if(app->in_run && app->pending_press &&
       furi_get_tick() - app->pending_press_tick >
           furi_ms_to_ticks(MACRO_PAD_DOUBLE_PRESS_MS)) {
        app->pending_press = false;
        view_dispatcher_send_custom_event(app->view_dispatcher, MacroPadEventPress);
    }
    if(app->in_run && app->test_gesture_pending && app->hid_connected) {
        app->test_gesture_pending = false;
        macro_pad_send_gesture(app, app->edit_gesture);
    }
    macro_pad_auto_transport_tick(app);
    macro_pad_wake_tick(app);
    macro_pad_refresh_run_view(app);
}

static MacroPadApp* macro_pad_app_alloc(void) {
    MacroPadApp* app = calloc(1, sizeof(MacroPadApp));
    app->last_action_gesture = MACRO_PAD_GESTURE_COUNT;
    macro_pad_settings_load(&app->settings);
    macro_pad_ble_identity_load(app);

    app->gui = furi_record_open(RECORD_GUI);
    app->notification = furi_record_open(RECORD_NOTIFICATION);
    app->input_events = furi_record_open(RECORD_INPUT_EVENTS);
    app->input_subscription =
        furi_pubsub_subscribe(app->input_events, macro_pad_input_activity_callback, app);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, macro_pad_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, macro_pad_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, macro_pad_tick_event_callback, 100U);
    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->config_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        MacroPadViewConfig,
        variable_item_list_get_view(app->config_list));

    app->gesture_list = variable_item_list_alloc();
    variable_item_list_set_wrap_around(app->gesture_list, false);
    view_dispatcher_add_view(
        app->view_dispatcher,
        MacroPadViewGesture,
        variable_item_list_get_view(app->gesture_list));

    app->run_view = view_alloc();
    view_allocate_model(app->run_view, ViewModelTypeLocking, sizeof(MacroPadViewModel));
    view_set_context(app->run_view, app);
    view_set_draw_callback(app->run_view, macro_pad_draw_callback);
    view_set_input_callback(app->run_view, macro_pad_run_input_callback);
    /*
     * On T-Embed this keeps normal rotation as Left/Right while held rotation
     * becomes Up/Down, so the two gestures remain distinguishable.
     */
    view_set_input_mode(app->run_view, ViewInputModeLeftRight);
    view_dispatcher_add_view(app->view_dispatcher, MacroPadViewRun, app->run_view);

    app->sequence_list = variable_item_list_alloc();
    variable_item_list_set_wrap_around(app->sequence_list, false);
    view_dispatcher_add_view(
        app->view_dispatcher,
        MacroPadViewSequence,
        variable_item_list_get_view(app->sequence_list));

    app->dialog = dialog_ex_alloc();
    dialog_ex_set_context(app->dialog, app);
    dialog_ex_set_result_callback(app->dialog, macro_pad_dialog_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, MacroPadViewDialog, dialog_ex_get_view(app->dialog));

    macro_pad_build_config(app);
    macro_pad_build_gesture_editor(app);
    macro_pad_build_sequence_editor(app);
    return app;
}

static void macro_pad_app_free(MacroPadApp* app) {
    notification_message(app->notification, &sequence_reset_rgb);
    macro_pad_wake_stop(app);
    macro_pad_transport_stop(app);
    if(app->settings_dirty) macro_pad_settings_save(&app->settings);

    furi_pubsub_unsubscribe(app->input_events, app->input_subscription);
    view_dispatcher_remove_view(app->view_dispatcher, MacroPadViewConfig);
    view_dispatcher_remove_view(app->view_dispatcher, MacroPadViewGesture);
    view_dispatcher_remove_view(app->view_dispatcher, MacroPadViewRun);
    view_dispatcher_remove_view(app->view_dispatcher, MacroPadViewSequence);
    view_dispatcher_remove_view(app->view_dispatcher, MacroPadViewDialog);
    variable_item_list_free(app->config_list);
    variable_item_list_free(app->gesture_list);
    variable_item_list_free(app->sequence_list);
    view_free(app->run_view);
    dialog_ex_free(app->dialog);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_INPUT_EVENTS);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t macro_pad_app(void* args) {
    UNUSED(args);
    MacroPadApp* app = macro_pad_app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, MacroPadViewConfig);
    view_dispatcher_run(app->view_dispatcher);
    macro_pad_app_free(app);
    return 0;
}
