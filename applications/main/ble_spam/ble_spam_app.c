#include "ble_spam_app.h"
#include "ble_uuid_db.h"
#include "views/ble_spam_view.h"
#include "views/ble_walk_scan_view.h"
#include "views/ble_walk_detail_view.h"
#include "views/ble_auto_walk_view.h"
#include "views/tracker_list_view.h"
#include "views/tracker_geiger_view.h"
#include "views/race_detector_view.h"
#include "views/ble_remote_view.h"
#include "ble_keyboard.h"
#include <dolphin/dolphin.h>
#include <furi_hal_power.h>
#include <furi_hal_rtc.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/text_input.h>
#include <notification/notification_messages.h>
#include <string.h>

#define BLE_HID_DIM_TIMEOUT_MS   (5UL * 60UL * 1000UL)
#define BLE_HID_SLEEP_TIMEOUT_MS (10UL * 60UL * 1000UL)

static void ble_spam_input_event_callback(const void* value, void* context) {
    furi_assert(value);
    furi_assert(context);

    BleSpamApp* app = context;
    const InputEvent* event = value;

    if(app->hid_wake_managed && event->type == InputTypePress) {
        app->hid_wake_last_activity_tick = furi_get_tick();
        app->hid_wake_rearm_requested = true;
    }

    if(app->hid_wake_managed && event->key == InputKeyOk && event->type == InputTypeLong) {
        app->hid_no_sleep_toggle_requested = true;
    }
}

void ble_spam_hid_wake_start(BleSpamApp* app) {
    furi_assert(app);

    if(!app->hid_wake_managed) {
        app->hid_no_sleep = false;
        if(!app->hid_insomnia_held) {
            furi_hal_power_insomnia_enter();
            app->hid_insomnia_held = true;
        }
    }

    app->hid_wake_last_activity_tick = furi_get_tick();
    app->hid_wake_rearm_requested = false;
    app->hid_no_sleep_toggle_requested = false;
    app->hid_wake_managed = true;
    if(!app->hid_wake_locked) {
        notification_message(app->notification, &sequence_display_backlight_enforce_on);
        app->hid_wake_locked = true;
    }
}

void ble_spam_hid_wake_stop(BleSpamApp* app) {
    furi_assert(app);

    app->hid_wake_managed = false;
    app->hid_wake_rearm_requested = false;
    app->hid_no_sleep_toggle_requested = false;
    app->hid_no_sleep = false;

    if(app->hid_wake_locked) {
        notification_message(app->notification, &sequence_display_backlight_enforce_auto);
        app->hid_wake_locked = false;
    }

    if(app->hid_insomnia_held) {
        furi_hal_power_insomnia_exit();
        app->hid_insomnia_held = false;
    }
}

static void ble_spam_hid_wake_tick(BleSpamApp* app) {
    if(!app->hid_wake_managed) {
        return;
    }

    if(app->hid_no_sleep_toggle_requested) {
        app->hid_no_sleep_toggle_requested = false;
        app->hid_no_sleep = !app->hid_no_sleep;
        app->hid_wake_last_activity_tick = furi_get_tick();
        app->hid_wake_rearm_requested = true;
        FURI_LOG_I(
            BLE_SPAM_LOG_TAG,
            "BLE HID no-sleep mode %s",
            app->hid_no_sleep ? "enabled" : "disabled");
    }

    if(app->hid_wake_rearm_requested) {
        app->hid_wake_rearm_requested = false;
        if(!app->hid_wake_locked) {
            notification_message(app->notification, &sequence_display_backlight_enforce_on);
            app->hid_wake_locked = true;
        }
    }

    if(app->hid_no_sleep) {
        return;
    }

    const uint32_t idle_ticks = furi_get_tick() - app->hid_wake_last_activity_tick;
    if(app->hid_wake_locked &&
       idle_ticks >= furi_ms_to_ticks(BLE_HID_DIM_TIMEOUT_MS)) {
        notification_message(app->notification, &sequence_display_backlight_enforce_auto);
        app->hid_wake_locked = false;
        notification_display_dim(app->notification);
        FURI_LOG_I(BLE_SPAM_LOG_TAG, "BLE HID idle for five minutes; display dimmed");
    }

    if(idle_ticks >= furi_ms_to_ticks(BLE_HID_SLEEP_TIMEOUT_MS)) {
        FURI_LOG_I(BLE_SPAM_LOG_TAG, "BLE HID idle for ten minutes; entering deep sleep");
        ble_spam_hid_wake_stop(app);

        if(furi_record_exists(RECORD_DOLPHIN)) {
            Dolphin* dolphin = furi_record_open(RECORD_DOLPHIN);
            dolphin_prepare_for_sleep(dolphin);
            furi_record_close(RECORD_DOLPHIN);
        }
        furi_hal_rtc_arm_resume();
        furi_hal_power_shutdown();
    }
}

static bool ble_spam_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    BleSpamApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool ble_spam_back_event_callback(void* context) {
    furi_assert(context);
    BleSpamApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void ble_spam_tick_event_callback(void* context) {
    furi_assert(context);
    BleSpamApp* app = context;
    ble_spam_hid_wake_tick(app);
    scene_manager_handle_tick_event(app->scene_manager);
}

static BleSpamApp* ble_spam_app_alloc(void) {
    BleSpamApp* app = malloc(sizeof(BleSpamApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->notification = furi_record_open(RECORD_NOTIFICATION);
    app->input_events = furi_record_open(RECORD_INPUT_EVENTS);
    app->hid_wake_last_activity_tick = 0;
    app->hid_wake_rearm_requested = false;
    app->hid_no_sleep_toggle_requested = false;
    app->hid_wake_managed = false;
    app->hid_wake_locked = false;
    app->hid_insomnia_held = false;
    app->hid_no_sleep = false;
    app->input_subscription =
        furi_pubsub_subscribe(app->input_events, ble_spam_input_event_callback, app);

    app->scene_manager = scene_manager_alloc(&ble_spam_scene_handlers, app);
    app->view_dispatcher = view_dispatcher_alloc();

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, ble_spam_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, ble_spam_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, ble_spam_tick_event_callback, 250);
    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Submenu
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewSubmenu, submenu_get_view(app->submenu));

    // Running view
    app->view_running = ble_spam_view_alloc();
    view_set_context(app->view_running, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewRunning, app->view_running);

    // BLE Walk views
    app->view_walk_scan = ble_walk_scan_view_alloc();
    view_set_context(app->view_walk_scan, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewWalkScan, app->view_walk_scan);

    app->view_walk_detail = ble_walk_detail_view_alloc();
    view_set_context(app->view_walk_detail, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewWalkDetail, app->view_walk_detail);

    // BLE Auto-Walk view
    app->view_auto_walk = ble_auto_walk_view_alloc();
    view_set_context(app->view_auto_walk, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewAutoWalk, app->view_auto_walk);

    // BLE Tracker views
    app->view_tracker_scan = tracker_list_view_alloc();
    view_set_context(app->view_tracker_scan, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewTrackerScan, app->view_tracker_scan);

    app->view_tracker_geiger = tracker_geiger_view_alloc();
    view_set_context(app->view_tracker_geiger, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewTrackerGeiger, app->view_tracker_geiger);

    app->tracker_geiger_timer = NULL;

    // Text input view for custom pair names
    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewTextInput, text_input_get_view(app->text_input));

    app->byte_input = byte_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewByteInput, byte_input_get_view(app->byte_input));

    // BLE RACE Detector view (CVE-2025-20700)
    app->view_race_detector = race_detector_view_alloc();
    view_set_context(app->view_race_detector, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewRaceDetector, app->view_race_detector);
    app->race_probe_abort = false;

    // BLE HID remote controls
    app->view_remote = ble_remote_view_alloc();
    view_set_context(app->view_remote, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, BleSpamViewRemote, app->view_remote);
    app->remote_mode = BleRemoteModePresenter;
    app->remote_active = false;
    app->remote_last_action_tick = 0;
    app->remote_jiggle_direction = 1;
    app->keyboard_hid = NULL;
    app->keyboard_hid_instance = NULL;
    memset(&app->keyboard_hid_config, 0, sizeof(app->keyboard_hid_config));
    app->keyboard_connected = false;
    app->keyboard_input_active = false;
    app->keyboard_append_enter = false;
    app->keyboard_text[0] = '\0';

    // State
    app->attack_type = BleSpamAttackAppleDevice;
    app->running = false;
    app->packet_count = 0;
    app->delay_ms = 100;
    app->current_index = 0;
    app->current_device[0] = '\0';
    app->custom_pair_name[0] = '\0';
    app->lovespouse_selection = BleSpamLovespouseRandom;
    app->lovespouse_value = 0;
    memset(app->lovespouse_custom_value, 0, sizeof(app->lovespouse_custom_value));

    return app;
}

static void ble_spam_app_free(BleSpamApp* app) {
    // Defensive cleanup in case the dispatcher is stopped while a keyboard
    // scene is active. This also restores the normal Bluetooth profile.
    ble_spam_hid_wake_stop(app);
    ble_keyboard_stop(app);

    furi_pubsub_unsubscribe(app->input_events, app->input_subscription);

    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewRunning);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewByteInput);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewWalkScan);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewWalkDetail);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewAutoWalk);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewTrackerScan);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewTrackerGeiger);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewRaceDetector);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewRemote);

    submenu_free(app->submenu);
    ble_spam_view_free(app->view_running);
    ble_walk_scan_view_free(app->view_walk_scan);
    ble_walk_detail_view_free(app->view_walk_detail);
    ble_auto_walk_view_free(app->view_auto_walk);
    tracker_list_view_free(app->view_tracker_scan);
    tracker_geiger_view_free(app->view_tracker_geiger);
    race_detector_view_free(app->view_race_detector);
    ble_remote_view_free(app->view_remote);
    text_input_free(app->text_input);
    byte_input_free(app->byte_input);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    app->gui = NULL;
    furi_record_close(RECORD_INPUT_EVENTS);
    app->input_events = NULL;
    furi_record_close(RECORD_NOTIFICATION);
    app->notification = NULL;

    free(app);
}

int32_t ble_spam_app(void* args) {
    UNUSED(args);
    ble_uuid_db_init();
    BleSpamApp* app = ble_spam_app_alloc();

    scene_manager_next_scene(app->scene_manager, BleSpamSceneMain);
    view_dispatcher_run(app->view_dispatcher);

    ble_spam_app_free(app);
    ble_uuid_db_deinit();
    return 0;
}
