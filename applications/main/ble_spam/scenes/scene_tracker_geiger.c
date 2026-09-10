#include "../ble_spam_app.h"
#include "../ble_tracker_hal.h"
#include "../views/tracker_geiger_view.h"

#include <furi_hal_speaker.h>
#include <string.h>

#define STALE_TIMEOUT_MS 4000
#define BEEP_DURATION_MS 50
#define TRACKER_IDLE_PERIOD_MS 1200U
#define TRACKER_NEAR_RSSI (-84)
#define TRACKER_CLOSE_RSSI (-45)
#define TRACKER_MIN_VOLUME 0.35f
#define TRACKER_GEIGER_EVENT_BEEP 0x400U

static uint32_t period_for_rssi(int8_t rssi) {
    if(rssi > -45) return 80;
    if(rssi > -60) return 160;
    if(rssi > -72) return 300;
    if(rssi > -84) return 600;
    return TRACKER_IDLE_PERIOD_MS;
}

static float pitch_for_rssi(int8_t rssi) {
    int32_t hz = 600 + ((int32_t)rssi + 100) * 30;
    if(hz < 600) hz = 600;
    if(hz > 3500) hz = 3500;
    return (float)hz;
}

static float volume_for_rssi(int8_t rssi) {
    if(rssi <= TRACKER_NEAR_RSSI) return 0.0f;
    if(rssi >= TRACKER_CLOSE_RSSI) return 1.0f;

    const float proximity =
        (float)(rssi - TRACKER_NEAR_RSSI) / (float)(TRACKER_CLOSE_RSSI - TRACKER_NEAR_RSSI);
    return TRACKER_MIN_VOLUME + proximity * (1.0f - TRACKER_MIN_VOLUME);
}

/* Furi timers run on the timer service thread, which cannot use speaker
 * ownership acquired by the UI thread. Defer all audio work to the scene. */
static void tracker_geiger_timer_callback(void* context) {
    BleSpamApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, TRACKER_GEIGER_EVENT_BEEP);
}

void ble_spam_scene_tracker_geiger_on_enter(void* context) {
    BleSpamApp* app = context;

    TrackerGeigerModel* model = view_get_model(app->view_tracker_geiger);
    memset(model, 0, sizeof(TrackerGeigerModel));
    memcpy(model->target_addr, app->tracker_target.addr, 6);
    model->target_kind = app->tracker_target.kind;
    strncpy(model->target_name, app->tracker_target.name, sizeof(model->target_name) - 1);
    model->target_name[sizeof(model->target_name) - 1] = '\0';
    model->rssi = app->tracker_target.rssi;
    model->last_seen_ms = furi_get_tick();
    model->stale = false;
    view_commit_model(app->view_tracker_geiger, true);

    app->tracker_current_rssi = app->tracker_target.rssi;
    app->tracker_current_stale = false;
    app->tracker_current_period = period_for_rssi(app->tracker_current_rssi);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewTrackerGeiger);

    // Re-arm scanner — picker scene stopped it on exit
    ble_tracker_hal_start_scan();

    app->tracker_geiger_timer =
        furi_timer_alloc(tracker_geiger_timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(app->tracker_geiger_timer, app->tracker_current_period);
}

bool ble_spam_scene_tracker_geiger_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom &&
        event.event == TRACKER_GEIGER_EVENT_BEEP) {
        const int8_t rssi = app->tracker_current_rssi;
        const bool stale = app->tracker_current_stale;
        const bool near = !stale && (rssi > TRACKER_NEAR_RSSI);
        const uint32_t period = stale ? TRACKER_IDLE_PERIOD_MS : period_for_rssi(rssi);

        if(near && furi_hal_speaker_acquire(0)) {
            furi_hal_speaker_start(pitch_for_rssi(rssi), volume_for_rssi(rssi));
            furi_delay_ms(BEEP_DURATION_MS);
            furi_hal_speaker_stop();
            furi_hal_speaker_release();
        }

        if(period != app->tracker_current_period) {
            app->tracker_current_period = period;
            furi_timer_restart(app->tracker_geiger_timer, period);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick) {
        uint16_t count;
        TrackerDevice* devices = ble_tracker_hal_get_devices(&count);

        // Find target by MAC
        TrackerDevice* match = NULL;
        for(uint16_t i = 0; i < count; i++) {
            if(memcmp(devices[i].addr, app->tracker_target.addr, 6) == 0) {
                match = &devices[i];
                break;
            }
        }

        TrackerGeigerModel* model = view_get_model(app->view_tracker_geiger);
        uint32_t now = furi_get_tick();

        if(match) {
            model->rssi = match->rssi;
            model->last_seen_ms = match->last_seen_ms;
        }
        bool stale = (now - model->last_seen_ms) > STALE_TIMEOUT_MS;
        model->stale = stale;
        view_commit_model(app->view_tracker_geiger, true);

        // Mirror to app state for timer-thread access
        app->tracker_current_rssi = model->rssi;
        app->tracker_current_stale = stale;
    }

    return consumed;
}

void ble_spam_scene_tracker_geiger_on_exit(void* context) {
    BleSpamApp* app = context;

    if(app->tracker_geiger_timer) {
        furi_timer_stop(app->tracker_geiger_timer);
        furi_timer_free(app->tracker_geiger_timer);
        app->tracker_geiger_timer = NULL;
    }

    ble_tracker_hal_stop_scan();
}
