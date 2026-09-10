#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MACRO_PAD_PROFILE_COUNT       (4U)
#define MACRO_PAD_GESTURE_COUNT       (5U)
#define MACRO_PAD_SEQUENCE_STEP_COUNT (4U)

typedef enum {
    MacroPadTransportBle,
    MacroPadTransportUsb,
    MacroPadTransportAuto,
    MacroPadTransportCount,
} MacroPadTransport;

typedef enum {
    MacroPadProfileEditing,
    MacroPadProfileMedia,
    MacroPadProfileStreaming,
    MacroPadProfileCad,
} MacroPadProfileId;

typedef enum {
    MacroPadGestureCcw,
    MacroPadGestureCw,
    MacroPadGesturePress,
    MacroPadGestureDoublePress,
    MacroPadGestureLongPress,
} MacroPadGesture;

typedef enum {
    MacroPadLedOff,
    MacroPadLedRed,
    MacroPadLedGreen,
    MacroPadLedBlue,
    MacroPadLedYellow,
    MacroPadLedCyan,
    MacroPadLedMagenta,
    MacroPadLedWhite,
    MacroPadLedCount,
} MacroPadLedColor;

typedef enum {
    MacroPadActionNone,
    MacroPadActionCopy,
    MacroPadActionPaste,
    MacroPadActionUndo,
    MacroPadActionRedo,
    MacroPadActionSelectAll,
    MacroPadActionSave,
    MacroPadActionFind,
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
    MacroPadActionPlayPause,
    MacroPadActionMute,
    MacroPadActionVolumeUp,
    MacroPadActionVolumeDown,
    MacroPadActionNextTrack,
    MacroPadActionPreviousTrack,
    MacroPadActionPageUp,
    MacroPadActionPageDown,
    MacroPadActionHome,
    MacroPadActionEnd,
    MacroPadActionBrowserBack,
    MacroPadActionBrowserForward,
    MacroPadActionCount,
} MacroPadAction;

typedef enum {
    MacroPadActionKindNone,
    MacroPadActionKindKeyboard,
    MacroPadActionKindConsumer,
} MacroPadActionKind;

typedef struct {
    const char* label;
    const char* short_label;
    MacroPadActionKind kind;
    uint16_t code;
} MacroPadActionDefinition;

typedef struct {
    uint8_t actions[MACRO_PAD_SEQUENCE_STEP_COUNT];
    uint16_t delays_ms[MACRO_PAD_SEQUENCE_STEP_COUNT - 1U];
} MacroPadSequence;

typedef struct {
    uint8_t led_color;
    MacroPadSequence gestures[MACRO_PAD_GESTURE_COUNT];
} MacroPadProfile;

typedef struct {
    uint8_t transport;
    uint8_t active_profile;
    bool stay_awake;
    MacroPadProfile profiles[MACRO_PAD_PROFILE_COUNT];
} MacroPadSettings;

const MacroPadActionDefinition* macro_pad_action_definition(MacroPadAction action);
const char* macro_pad_transport_label(MacroPadTransport transport);
const char* macro_pad_profile_label(MacroPadProfileId profile);
const char* macro_pad_gesture_label(MacroPadGesture gesture);
const char* macro_pad_led_label(MacroPadLedColor color);
void macro_pad_settings_defaults(MacroPadSettings* settings);
void macro_pad_settings_reset_profile(MacroPadSettings* settings, MacroPadProfileId profile);
void macro_pad_settings_load(MacroPadSettings* settings);
bool macro_pad_settings_save(const MacroPadSettings* settings);
bool macro_pad_settings_export(const MacroPadSettings* settings);
bool macro_pad_settings_import(MacroPadSettings* settings);
const char* macro_pad_settings_export_path(void);
