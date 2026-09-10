#include "macro_model.h"

#include <flipper_format/flipper_format.h>
#include <furi.h>
#include <furi_hal_usb_hid.h>
#include <storage/storage.h>
#include <toolbox/saved_struct.h>
#include <stdio.h>
#include <string.h>

#define TAG "MacroPadModel"

#define MACRO_PAD_FOLDER                    EXT_PATH("macro")
#define MACRO_PAD_SETTINGS_PATH              MACRO_PAD_FOLDER "/default.macro"
#define MACRO_PAD_EXPORT_PATH                MACRO_PAD_FOLDER "/macro_pad_export.macro"
#define MACRO_PAD_FILE_TYPE                  "BLE/USB Macro Pad"
#define MACRO_PAD_FILE_VERSION               (3U)
#define MACRO_PAD_FILE_VERSION_3_SLOTS       (2U)
#define MACRO_PAD_FILE_VERSION_5_SLOTS       (1U)
#define MACRO_PAD_LEGACY_SLOT_COUNT          (5U)
#define MACRO_PAD_LEGACY_SLOT_UP             (0U)
#define MACRO_PAD_LEGACY_SLOT_DOWN           (1U)
#define MACRO_PAD_LEGACY_SLOT_OK             (4U)
#define MACRO_PAD_LEGACY_PRESET_COUNT        (4U)
#define MACRO_PAD_LEGACY_SETTINGS_PATH       INT_PATH(".ble_macro.settings")
#define MACRO_PAD_LEGACY_SETTINGS_MAGIC      (0x4D)
#define MACRO_PAD_LEGACY_SETTINGS_VER        (1)
#define MACRO_PAD_PROFILE_ACTION_VALUE_COUNT \
    (MACRO_PAD_GESTURE_COUNT * MACRO_PAD_SEQUENCE_STEP_COUNT)
#define MACRO_PAD_PROFILE_DELAY_VALUE_COUNT \
    (MACRO_PAD_GESTURE_COUNT * (MACRO_PAD_SEQUENCE_STEP_COUNT - 1U))

typedef struct {
    uint8_t preset;
    uint8_t actions[MACRO_PAD_LEGACY_SLOT_COUNT];
} MacroPadLegacySettings;

static const MacroPadActionDefinition macro_pad_actions[MacroPadActionCount] = {
    [MacroPadActionNone] = {"None", "None", MacroPadActionKindNone, 0},
    [MacroPadActionCopy] =
        {"Copy", "Copy", MacroPadActionKindKeyboard, KEY_MOD_LEFT_CTRL | HID_KEYBOARD_C},
    [MacroPadActionPaste] =
        {"Paste", "Paste", MacroPadActionKindKeyboard, KEY_MOD_LEFT_CTRL | HID_KEYBOARD_V},
    [MacroPadActionUndo] =
        {"Undo", "Undo", MacroPadActionKindKeyboard, KEY_MOD_LEFT_CTRL | HID_KEYBOARD_Z},
    [MacroPadActionRedo] =
        {"Redo", "Redo", MacroPadActionKindKeyboard, KEY_MOD_LEFT_CTRL | HID_KEYBOARD_Y},
    [MacroPadActionSelectAll] =
        {"Select All", "All", MacroPadActionKindKeyboard, KEY_MOD_LEFT_CTRL | HID_KEYBOARD_A},
    [MacroPadActionSave] =
        {"Save", "Save", MacroPadActionKindKeyboard, KEY_MOD_LEFT_CTRL | HID_KEYBOARD_S},
    [MacroPadActionFind] =
        {"Find", "Find", MacroPadActionKindKeyboard, KEY_MOD_LEFT_CTRL | HID_KEYBOARD_F},
    [MacroPadActionMacCopy] =
        {"Mac Copy", "M.Copy", MacroPadActionKindKeyboard, KEY_MOD_LEFT_GUI | HID_KEYBOARD_C},
    [MacroPadActionMacPaste] =
        {"Mac Paste", "M.Paste", MacroPadActionKindKeyboard, KEY_MOD_LEFT_GUI | HID_KEYBOARD_V},
    [MacroPadActionMacUndo] =
        {"Mac Undo", "M.Undo", MacroPadActionKindKeyboard, KEY_MOD_LEFT_GUI | HID_KEYBOARD_Z},
    [MacroPadActionMacRedo] = {
        "Mac Redo",
        "M.Redo",
        MacroPadActionKindKeyboard,
        KEY_MOD_LEFT_GUI | KEY_MOD_LEFT_SHIFT | HID_KEYBOARD_Z,
    },
    [MacroPadActionMacSelectAll] = {
        "Mac Select All",
        "M.All",
        MacroPadActionKindKeyboard,
        KEY_MOD_LEFT_GUI | HID_KEYBOARD_A,
    },
    [MacroPadActionMacSave] =
        {"Mac Save", "M.Save", MacroPadActionKindKeyboard, KEY_MOD_LEFT_GUI | HID_KEYBOARD_S},
    [MacroPadActionMacFind] =
        {"Mac Find", "M.Find", MacroPadActionKindKeyboard, KEY_MOD_LEFT_GUI | HID_KEYBOARD_F},
    [MacroPadActionEnter] =
        {"Enter", "Enter", MacroPadActionKindKeyboard, HID_KEYBOARD_RETURN},
    [MacroPadActionEscape] =
        {"Escape", "Esc", MacroPadActionKindKeyboard, HID_KEYBOARD_ESCAPE},
    [MacroPadActionTab] = {"Tab", "Tab", MacroPadActionKindKeyboard, HID_KEYBOARD_TAB},
    [MacroPadActionSpace] =
        {"Space", "Space", MacroPadActionKindKeyboard, HID_KEYBOARD_SPACEBAR},
    [MacroPadActionBackspace] =
        {"Backspace", "Bksp", MacroPadActionKindKeyboard, HID_KEYBOARD_DELETE},
    [MacroPadActionScreenshot] =
        {"Screenshot", "Screen", MacroPadActionKindKeyboard, HID_KEYBOARD_PRINT_SCREEN},
    [MacroPadActionPlayPause] =
        {"Play/Pause", "Play", MacroPadActionKindConsumer, HID_CONSUMER_PLAY_PAUSE},
    [MacroPadActionMute] =
        {"Mute", "Mute", MacroPadActionKindConsumer, HID_CONSUMER_MUTE},
    [MacroPadActionVolumeUp] =
        {"Volume +", "Vol+", MacroPadActionKindConsumer, HID_CONSUMER_VOLUME_INCREMENT},
    [MacroPadActionVolumeDown] =
        {"Volume -", "Vol-", MacroPadActionKindConsumer, HID_CONSUMER_VOLUME_DECREMENT},
    [MacroPadActionNextTrack] =
        {"Next Track", "Next", MacroPadActionKindConsumer, HID_CONSUMER_SCAN_NEXT_TRACK},
    [MacroPadActionPreviousTrack] = {
        "Previous Track",
        "Prev",
        MacroPadActionKindConsumer,
        HID_CONSUMER_SCAN_PREVIOUS_TRACK,
    },
    [MacroPadActionPageUp] =
        {"Page Up", "PgUp", MacroPadActionKindKeyboard, HID_KEYBOARD_PAGE_UP},
    [MacroPadActionPageDown] =
        {"Page Down", "PgDn", MacroPadActionKindKeyboard, HID_KEYBOARD_PAGE_DOWN},
    [MacroPadActionHome] =
        {"Home", "Home", MacroPadActionKindKeyboard, HID_KEYBOARD_HOME},
    [MacroPadActionEnd] =
        {"End", "End", MacroPadActionKindKeyboard, HID_KEYBOARD_END},
    [MacroPadActionBrowserBack] =
        {"Browser Back", "Back", MacroPadActionKindConsumer, HID_CONSUMER_AC_BACK},
    [MacroPadActionBrowserForward] =
        {"Browser Forward", "Fwd", MacroPadActionKindConsumer, HID_CONSUMER_AC_FORWARD},
};

static const char* const macro_pad_transport_labels[MacroPadTransportCount] = {
    [MacroPadTransportBle] = "BLE HID",
    [MacroPadTransportUsb] = "USB HID",
    [MacroPadTransportAuto] = "Auto USB/BLE",
};

static const char* const macro_pad_profile_labels[MACRO_PAD_PROFILE_COUNT] = {
    [MacroPadProfileEditing] = "Editing",
    [MacroPadProfileMedia] = "Media",
    [MacroPadProfileStreaming] = "Streaming",
    [MacroPadProfileCad] = "CAD",
};

static const char* const macro_pad_gesture_labels[MACRO_PAD_GESTURE_COUNT] = {
    [MacroPadGestureCcw] = "Turn CCW",
    [MacroPadGestureCw] = "Turn CW",
    [MacroPadGesturePress] = "Press",
    [MacroPadGestureDoublePress] = "Double Press",
    [MacroPadGestureLongPress] = "Long Press",
};

static const char* const macro_pad_led_labels[MacroPadLedCount] = {
    [MacroPadLedOff] = "Off",
    [MacroPadLedRed] = "Red",
    [MacroPadLedGreen] = "Green",
    [MacroPadLedBlue] = "Blue",
    [MacroPadLedYellow] = "Yellow",
    [MacroPadLedCyan] = "Cyan",
    [MacroPadLedMagenta] = "Magenta",
    [MacroPadLedWhite] = "White",
};

const MacroPadActionDefinition* macro_pad_action_definition(MacroPadAction action) {
    if(action >= MacroPadActionCount) action = MacroPadActionNone;
    return &macro_pad_actions[action];
}

const char* macro_pad_transport_label(MacroPadTransport transport) {
    if(transport >= MacroPadTransportCount) transport = MacroPadTransportBle;
    return macro_pad_transport_labels[transport];
}

const char* macro_pad_profile_label(MacroPadProfileId profile) {
    if(profile >= MACRO_PAD_PROFILE_COUNT) profile = MacroPadProfileEditing;
    return macro_pad_profile_labels[profile];
}

const char* macro_pad_gesture_label(MacroPadGesture gesture) {
    if(gesture >= MACRO_PAD_GESTURE_COUNT) gesture = MacroPadGesturePress;
    return macro_pad_gesture_labels[gesture];
}

const char* macro_pad_led_label(MacroPadLedColor color) {
    if(color >= MacroPadLedCount) color = MacroPadLedOff;
    return macro_pad_led_labels[color];
}

static void macro_pad_sequence_set_first(MacroPadSequence* sequence, MacroPadAction action) {
    sequence->actions[0] = action;
}

void macro_pad_settings_reset_profile(MacroPadSettings* settings, MacroPadProfileId profile) {
    furi_assert(settings);
    if(profile >= MACRO_PAD_PROFILE_COUNT) return;

    MacroPadProfile* target = &settings->profiles[profile];
    memset(target, 0, sizeof(MacroPadProfile));
    for(uint8_t gesture = 0; gesture < MACRO_PAD_GESTURE_COUNT; gesture++) {
        for(uint8_t delay = 0; delay < MACRO_PAD_SEQUENCE_STEP_COUNT - 1U; delay++) {
            target->gestures[gesture].delays_ms[delay] = 100U;
        }
    }

    switch(profile) {
    case MacroPadProfileEditing:
        target->led_color = MacroPadLedBlue;
        macro_pad_sequence_set_first(&target->gestures[MacroPadGestureCcw], MacroPadActionCopy);
        macro_pad_sequence_set_first(&target->gestures[MacroPadGestureCw], MacroPadActionPaste);
        macro_pad_sequence_set_first(&target->gestures[MacroPadGesturePress], MacroPadActionEnter);
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGestureDoublePress], MacroPadActionUndo);
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGestureLongPress], MacroPadActionSave);
        break;
    case MacroPadProfileMedia:
        target->led_color = MacroPadLedGreen;
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGestureCcw], MacroPadActionVolumeDown);
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGestureCw], MacroPadActionVolumeUp);
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGesturePress], MacroPadActionPlayPause);
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGestureDoublePress], MacroPadActionNextTrack);
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGestureLongPress], MacroPadActionMute);
        break;
    case MacroPadProfileStreaming:
        target->led_color = MacroPadLedMagenta;
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGestureCcw], MacroPadActionVolumeDown);
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGestureCw], MacroPadActionVolumeUp);
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGesturePress], MacroPadActionMute);
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGestureDoublePress], MacroPadActionPlayPause);
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGestureLongPress], MacroPadActionScreenshot);
        break;
    case MacroPadProfileCad:
        target->led_color = MacroPadLedYellow;
        macro_pad_sequence_set_first(&target->gestures[MacroPadGestureCcw], MacroPadActionUndo);
        macro_pad_sequence_set_first(&target->gestures[MacroPadGestureCw], MacroPadActionRedo);
        macro_pad_sequence_set_first(&target->gestures[MacroPadGesturePress], MacroPadActionEnter);
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGestureDoublePress], MacroPadActionEscape);
        macro_pad_sequence_set_first(
            &target->gestures[MacroPadGestureLongPress], MacroPadActionSave);
        break;
    default:
        break;
    }
}

void macro_pad_settings_defaults(MacroPadSettings* settings) {
    furi_assert(settings);
    memset(settings, 0, sizeof(MacroPadSettings));
    settings->transport = MacroPadTransportBle;
    settings->active_profile = MacroPadProfileEditing;
    settings->stay_awake = true;
    for(uint8_t profile = 0; profile < MACRO_PAD_PROFILE_COUNT; profile++) {
        macro_pad_settings_reset_profile(settings, profile);
    }
}

static bool macro_pad_settings_valid(const MacroPadSettings* settings) {
    if(settings->transport >= MacroPadTransportCount ||
       settings->active_profile >= MACRO_PAD_PROFILE_COUNT) {
        return false;
    }

    for(uint8_t profile = 0; profile < MACRO_PAD_PROFILE_COUNT; profile++) {
        const MacroPadProfile* item = &settings->profiles[profile];
        if(item->led_color >= MacroPadLedCount) return false;
        for(uint8_t gesture = 0; gesture < MACRO_PAD_GESTURE_COUNT; gesture++) {
            const MacroPadSequence* sequence = &item->gestures[gesture];
            for(uint8_t step = 0; step < MACRO_PAD_SEQUENCE_STEP_COUNT; step++) {
                if(sequence->actions[step] >= MacroPadActionCount) return false;
            }
            for(uint8_t delay = 0; delay < MACRO_PAD_SEQUENCE_STEP_COUNT - 1U; delay++) {
                if(sequence->delays_ms[delay] > 10000U) return false;
            }
        }
    }
    return true;
}

static bool macro_pad_settings_read_v3(FlipperFormat* file, MacroPadSettings* settings) {
    MacroPadSettings candidate;
    macro_pad_settings_defaults(&candidate);

    uint32_t transport = 0;
    uint32_t active_profile = 0;
    uint32_t stay_awake = 1;
    if(!flipper_format_read_uint32(file, "Transport", &transport, 1) ||
       !flipper_format_read_uint32(file, "ActiveProfile", &active_profile, 1) ||
       !flipper_format_read_uint32(file, "StayAwake", &stay_awake, 1)) {
        return false;
    }

    candidate.transport = transport;
    candidate.active_profile = active_profile;
    candidate.stay_awake = stay_awake != 0;

    for(uint8_t profile = 0; profile < MACRO_PAD_PROFILE_COUNT; profile++) {
        char key[24];
        uint32_t led = 0;
        uint32_t actions[MACRO_PAD_PROFILE_ACTION_VALUE_COUNT];
        uint32_t delays[MACRO_PAD_PROFILE_DELAY_VALUE_COUNT];

        snprintf(key, sizeof(key), "Profile%uLed", profile);
        if(!flipper_format_read_uint32(file, key, &led, 1)) return false;
        snprintf(key, sizeof(key), "Profile%uActions", profile);
        if(!flipper_format_read_uint32(
               file, key, actions, MACRO_PAD_PROFILE_ACTION_VALUE_COUNT)) {
            return false;
        }
        snprintf(key, sizeof(key), "Profile%uDelays", profile);
        if(!flipper_format_read_uint32(
               file, key, delays, MACRO_PAD_PROFILE_DELAY_VALUE_COUNT)) {
            return false;
        }

        candidate.profiles[profile].led_color = led;
        for(uint8_t gesture = 0; gesture < MACRO_PAD_GESTURE_COUNT; gesture++) {
            for(uint8_t step = 0; step < MACRO_PAD_SEQUENCE_STEP_COUNT; step++) {
                const uint8_t index = gesture * MACRO_PAD_SEQUENCE_STEP_COUNT + step;
                candidate.profiles[profile].gestures[gesture].actions[step] = actions[index];
            }
            for(uint8_t delay = 0; delay < MACRO_PAD_SEQUENCE_STEP_COUNT - 1U; delay++) {
                const uint8_t index =
                    gesture * (MACRO_PAD_SEQUENCE_STEP_COUNT - 1U) + delay;
                candidate.profiles[profile].gestures[gesture].delays_ms[delay] = delays[index];
            }
        }
    }

    if(!macro_pad_settings_valid(&candidate)) return false;
    *settings = candidate;
    return true;
}

static bool macro_pad_settings_read_legacy(
    FlipperFormat* file,
    uint32_t file_version,
    MacroPadSettings* settings) {
    uint32_t preset = 0;
    uint32_t transport = MacroPadTransportBle;
    uint32_t actions[MACRO_PAD_LEGACY_SLOT_COUNT] = {0};
    if(!flipper_format_read_uint32(file, "Preset", &preset, 1)) return false;

    const uint32_t action_count = file_version == MACRO_PAD_FILE_VERSION_3_SLOTS ?
                                      3U :
                                      MACRO_PAD_LEGACY_SLOT_COUNT;
    if(!flipper_format_read_uint32(file, "Actions", actions, action_count)) return false;
    flipper_format_read_uint32(file, "Transport", &transport, 1);
    if(transport > MacroPadTransportUsb || preset >= MACRO_PAD_LEGACY_PRESET_COUNT) {
        return false;
    }
    for(uint8_t index = 0; index < action_count; index++) {
        if(actions[index] >= MacroPadActionCount) return false;
    }

    MacroPadSettings candidate;
    macro_pad_settings_defaults(&candidate);
    candidate.transport = transport;
    MacroPadProfile* editing = &candidate.profiles[MacroPadProfileEditing];
    if(file_version == MACRO_PAD_FILE_VERSION_5_SLOTS) {
        editing->gestures[MacroPadGestureCcw].actions[0] = actions[MACRO_PAD_LEGACY_SLOT_UP];
        editing->gestures[MacroPadGestureCw].actions[0] = actions[MACRO_PAD_LEGACY_SLOT_DOWN];
        editing->gestures[MacroPadGesturePress].actions[0] = actions[MACRO_PAD_LEGACY_SLOT_OK];
    } else {
        editing->gestures[MacroPadGestureCcw].actions[0] = actions[0];
        editing->gestures[MacroPadGestureCw].actions[0] = actions[1];
        editing->gestures[MacroPadGesturePress].actions[0] = actions[2];
    }

    if(!macro_pad_settings_valid(&candidate)) return false;
    *settings = candidate;
    return true;
}

static bool macro_pad_settings_load_path(
    MacroPadSettings* settings,
    const char* path,
    bool allow_legacy,
    bool* needs_upgrade) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* file_type = furi_string_alloc();
    uint32_t file_version = 0;
    bool loaded = false;
    *needs_upgrade = false;

    do {
        if(!storage_simply_mkdir(storage, MACRO_PAD_FOLDER)) break;
        if(!flipper_format_file_open_existing(file, path)) break;
        if(!flipper_format_read_header(file, file_type, &file_version)) break;
        if(!furi_string_equal_str(file_type, MACRO_PAD_FILE_TYPE) &&
           !(allow_legacy && furi_string_equal_str(file_type, "BLE Macro Pad"))) {
            break;
        }

        if(file_version == MACRO_PAD_FILE_VERSION) {
            loaded = macro_pad_settings_read_v3(file, settings);
        } else if(
            allow_legacy && (file_version == MACRO_PAD_FILE_VERSION_3_SLOTS ||
                             file_version == MACRO_PAD_FILE_VERSION_5_SLOTS)) {
            loaded = macro_pad_settings_read_legacy(file, file_version, settings);
            *needs_upgrade = loaded;
        }
    } while(false);

    flipper_format_file_close(file);
    furi_string_free(file_type);
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    return loaded;
}

static bool macro_pad_settings_save_path(const MacroPadSettings* settings, const char* path) {
    if(!macro_pad_settings_valid(settings)) {
        FURI_LOG_E(TAG, "Refusing to save invalid settings");
        return false;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    uint32_t transport = settings->transport;
    uint32_t active_profile = settings->active_profile;
    uint32_t stay_awake = settings->stay_awake ? 1U : 0U;
    bool saved = false;

    do {
        if(!storage_simply_mkdir(storage, MACRO_PAD_FOLDER)) break;
        if(!flipper_format_file_open_always(file, path)) break;
        if(!flipper_format_write_header_cstr(
               file, MACRO_PAD_FILE_TYPE, MACRO_PAD_FILE_VERSION)) {
            break;
        }
        if(!flipper_format_write_uint32(file, "Transport", &transport, 1) ||
           !flipper_format_write_uint32(file, "ActiveProfile", &active_profile, 1) ||
           !flipper_format_write_uint32(file, "StayAwake", &stay_awake, 1)) {
            break;
        }

        bool profiles_saved = true;
        for(uint8_t profile = 0; profile < MACRO_PAD_PROFILE_COUNT; profile++) {
            char key[24];
            uint32_t led = settings->profiles[profile].led_color;
            uint32_t actions[MACRO_PAD_PROFILE_ACTION_VALUE_COUNT];
            uint32_t delays[MACRO_PAD_PROFILE_DELAY_VALUE_COUNT];

            for(uint8_t gesture = 0; gesture < MACRO_PAD_GESTURE_COUNT; gesture++) {
                for(uint8_t step = 0; step < MACRO_PAD_SEQUENCE_STEP_COUNT; step++) {
                    const uint8_t index = gesture * MACRO_PAD_SEQUENCE_STEP_COUNT + step;
                    actions[index] = settings->profiles[profile].gestures[gesture].actions[step];
                }
                for(uint8_t delay = 0; delay < MACRO_PAD_SEQUENCE_STEP_COUNT - 1U; delay++) {
                    const uint8_t index =
                        gesture * (MACRO_PAD_SEQUENCE_STEP_COUNT - 1U) + delay;
                    delays[index] = settings->profiles[profile].gestures[gesture].delays_ms[delay];
                }
            }

            snprintf(key, sizeof(key), "Profile%uLed", profile);
            profiles_saved &= flipper_format_write_uint32(file, key, &led, 1);
            snprintf(key, sizeof(key), "Profile%uActions", profile);
            profiles_saved &= flipper_format_write_uint32(
                file, key, actions, MACRO_PAD_PROFILE_ACTION_VALUE_COUNT);
            snprintf(key, sizeof(key), "Profile%uDelays", profile);
            profiles_saved &= flipper_format_write_uint32(
                file, key, delays, MACRO_PAD_PROFILE_DELAY_VALUE_COUNT);
            if(!profiles_saved) break;
        }
        if(!profiles_saved) break;
        saved = true;
    } while(false);

    flipper_format_file_close(file);
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    if(!saved) FURI_LOG_E(TAG, "Failed to save %s", path);
    return saved;
}

void macro_pad_settings_load(MacroPadSettings* settings) {
    furi_assert(settings);
    macro_pad_settings_defaults(settings);

    bool needs_upgrade = false;
    if(macro_pad_settings_load_path(settings, MACRO_PAD_SETTINGS_PATH, true, &needs_upgrade)) {
        if(needs_upgrade && macro_pad_settings_save(settings)) {
            FURI_LOG_I(TAG, "Migrated Macro Pad settings to version %u", MACRO_PAD_FILE_VERSION);
        }
        return;
    }

    MacroPadLegacySettings legacy;
    if(saved_struct_load(
           MACRO_PAD_LEGACY_SETTINGS_PATH,
           &legacy,
           sizeof(legacy),
           MACRO_PAD_LEGACY_SETTINGS_MAGIC,
           MACRO_PAD_LEGACY_SETTINGS_VER) &&
       legacy.preset < MACRO_PAD_LEGACY_PRESET_COUNT) {
        bool valid = true;
        for(uint8_t index = 0; index < MACRO_PAD_LEGACY_SLOT_COUNT; index++) {
            valid &= legacy.actions[index] < MacroPadActionCount;
        }
        if(valid) {
            MacroPadProfile* editing = &settings->profiles[MacroPadProfileEditing];
            editing->gestures[MacroPadGestureCcw].actions[0] =
                legacy.actions[MACRO_PAD_LEGACY_SLOT_UP];
            editing->gestures[MacroPadGestureCw].actions[0] =
                legacy.actions[MACRO_PAD_LEGACY_SLOT_DOWN];
            editing->gestures[MacroPadGesturePress].actions[0] =
                legacy.actions[MACRO_PAD_LEGACY_SLOT_OK];
            macro_pad_settings_save(settings);
            FURI_LOG_I(TAG, "Migrated legacy Macro Pad settings");
            return;
        }
    }

    FURI_LOG_D(TAG, "No valid saved settings; using Macro Pad defaults");
}

bool macro_pad_settings_save(const MacroPadSettings* settings) {
    furi_assert(settings);
    return macro_pad_settings_save_path(settings, MACRO_PAD_SETTINGS_PATH);
}

bool macro_pad_settings_export(const MacroPadSettings* settings) {
    furi_assert(settings);
    return macro_pad_settings_save_path(settings, MACRO_PAD_EXPORT_PATH);
}

bool macro_pad_settings_import(MacroPadSettings* settings) {
    furi_assert(settings);
    bool needs_upgrade = false;
    return macro_pad_settings_load_path(
        settings, MACRO_PAD_EXPORT_PATH, false, &needs_upgrade);
}

const char* macro_pad_settings_export_path(void) {
    return MACRO_PAD_EXPORT_PATH;
}
