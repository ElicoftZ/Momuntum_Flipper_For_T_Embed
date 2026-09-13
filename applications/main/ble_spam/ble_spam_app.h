#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <input/input.h>
#include <notification/notification.h>

#include "../bad_usb/helpers/bad_usb_hid.h"
#include "ble_tracker_hal.h"
#include "scenes/scenes.h"
#include "views/ble_remote_view.h"

#define BLE_SPAM_LOG_TAG "BleSpam"

typedef enum {
    BleSpamAttackAppleDevice,
    BleSpamAttackAppleAction,
    BleSpamAttackAppleNotYourDevice,
    BleSpamAttackFastPair,
    BleSpamAttackSwiftPair,
    BleSpamAttackSamsungBuds,
    BleSpamAttackSamsungWatch,
    BleSpamAttackXiaomi,
    BleSpamAttackNameflood,
    BleSpamAttackLovespouse,
    BleSpamAttackPairSpam,
    BleSpamAttackPairSpamRickroll,
    BleSpamAttackPairSpamCustom,
    BleSpamAttackSourApple,
    BleSpamAttackAppleJuice,
    BleSpamAttackFlipperZero,
    BleSpamAttackCount,
} BleSpamAttackType;

typedef enum {
    BleSpamLovespouseRandom,
    BleSpamLovespouseValue,
    BleSpamLovespouseBruteforce,
} BleSpamLovespouseSelection;

typedef enum {
    BleSpamCustomEventToggle = 100,
    BleSpamCustomEventSpeedUp,
    BleSpamCustomEventSpeedDown,
} BleSpamCustomEvent;

typedef enum {
    BleSpamCustomEventWalkConnect = 200,
} BleSpamWalkEvent;

typedef enum {
    BleSpamViewSubmenu,
    BleSpamViewRunning,
    BleSpamViewTextInput,
    BleSpamViewByteInput,
    BleSpamViewWalkScan,
    BleSpamViewWalkDetail,
    BleSpamViewAutoWalk,
    BleSpamViewTrackerScan,
    BleSpamViewTrackerGeiger,
    BleSpamViewRaceDetector,
    BleSpamViewRemote,
    BleSpamViewWhisperPair,
} BleSpamViewId;

typedef struct {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;

    Submenu* submenu;
    View* view_running;
    TextInput* text_input;
    ByteInput* byte_input;

    // Attack state
    BleSpamAttackType attack_type;
    volatile bool running;
    uint32_t packet_count;
    uint32_t delay_ms;
    uint16_t current_index;
    char current_device[48];
    char custom_pair_name[32];
    BleSpamLovespouseSelection lovespouse_selection;
    uint32_t lovespouse_value;
    uint8_t lovespouse_custom_value[3];

    // BLE Walk state
    View* view_walk_scan;
    View* view_walk_detail;
    uint16_t walk_selected_device;
    uint16_t walk_selected_service;
    uint16_t walk_selected_char;

    // BLE Auto-Walk state
    View* view_auto_walk;

    // BLE Tracker state
    View* view_tracker_scan;
    View* view_tracker_geiger;
    TrackerDevice tracker_target;
    FuriTimer* tracker_geiger_timer;
    volatile int8_t tracker_current_rssi;
    volatile bool tracker_current_stale;
    uint32_t tracker_current_period;

    // Airoha RACE Detector state (CVE-2025-20700)
    View* view_race_detector;
    volatile bool race_probe_abort;

    struct WhisperPair* whisper_pair;

    // BLE keyboard state. The HID profile stays alive while the text-input
    // scene is open and is torn down before any other Bluetooth tool runs.
    const BadUsbHidApi* keyboard_hid;
    void* keyboard_hid_instance;
    BadUsbHidConfig keyboard_hid_config;
    volatile bool keyboard_connected;
    bool keyboard_input_active;
    bool keyboard_append_enter;
    char keyboard_text[128];

    // Everyday BLE HID remotes share the same safe HID lifecycle as the
    // keyboard and release it before scanners or testing tools can run.
    View* view_remote;
    BleRemoteMode remote_mode;
    bool remote_active;
    uint32_t remote_last_action_tick;
    int8_t remote_jiggle_direction;

    // Keep everyday HID controls readable for five minutes after the latest
    // button press, then return the display to its normal dim/sleep policy.
    NotificationApp* notification;
    FuriPubSub* input_events;
    FuriPubSubSubscription* input_subscription;
    volatile uint32_t hid_wake_last_activity_tick;
    volatile bool hid_wake_rearm_requested;
    volatile bool hid_no_sleep_toggle_requested;
    bool hid_wake_managed;
    bool hid_wake_locked;
    bool hid_insomnia_held;
    bool hid_no_sleep;
} BleSpamApp;

void ble_spam_hid_wake_start(BleSpamApp* app);
void ble_spam_hid_wake_stop(BleSpamApp* app);
