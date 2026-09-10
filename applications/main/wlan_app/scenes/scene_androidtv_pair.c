/* Android TV step 2: pair with the TV.
 *
 * Reached from the remote scene when the box doesn't recognise our client
 * certificate yet. wlan_androidtv_op_pair_start() opens the Polo pairing
 * channel (generating the client cert on first use — a few seconds), the TV
 * shows a 6-hex-char PIN, the user types it, and op_pair_finish() proves
 * knowledge of it. On success we pop back to the remote scene, which then
 * connects with the now-trusted certificate. */

#include "../wlan_app.h"
#include "../wlan_androidtv.h"

#include <string.h>
#include <stdio.h>

typedef enum {
    AtvPairStarting = 0, // op_pair_start running (may include cert generation)
    AtvPairPin, // TV shows the PIN, waiting for the user
    AtvPairVerifying, // op_pair_finish running
    AtvPairError,
} AtvPairPhase;

static AtvPairPhase s_phase;

static void atv_pair_show_popup(WlanApp* app, const char* header, const char* text) {
    popup_reset(app->popup);
    popup_set_header(app->popup, header, 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, text, 64, 34, AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
}

static void atv_pair_pin_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventAndroidTvPinDone);
}

static void atv_pair_show_pin_input(WlanApp* app) {
    TextInput* ti = app->text_input;
    text_input_reset(ti);
    text_input_set_validator(ti, NULL, NULL);
    text_input_set_header_text(ti, "Enter PIN from TV");
    text_input_set_minimum_length(ti, WLAN_ATV_PIN_LEN);
    app->androidtv_pin[0] = '\0';
    text_input_set_result_callback(
        ti, atv_pair_pin_cb, app, app->androidtv_pin, sizeof(app->androidtv_pin), true);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewTextInput);
}

static void atv_pair_retry_button_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventAndroidTvRetry);
    }
}

static void atv_pair_show_error(WlanApp* app, const char* msg) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 14, AlignCenter, AlignTop, FontPrimary, "Pairing failed");
    widget_add_string_element(app->widget, 64, 34, AlignCenter, AlignCenter, FontSecondary, msg);
    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Retry", atv_pair_retry_button_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

static void atv_pair_start(WlanApp* app) {
    if(!app->androidtv) app->androidtv = wlan_androidtv_alloc();
    if(!app->androidtv) {
        s_phase = AtvPairError;
        atv_pair_show_error(app, "Out of memory");
        return;
    }
    s_phase = AtvPairStarting;
    atv_pair_show_popup(app, "Pairing", "Look at your TV...");
    wlan_androidtv_op_pair_start(app->androidtv, app->androidtv_ip);
}

void wlan_app_scene_androidtv_pair_on_enter(void* context) {
    WlanApp* app = context;
    atv_pair_start(app);
}

bool wlan_app_scene_androidtv_pair_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventAndroidTvPinDone) {
            s_phase = AtvPairVerifying;
            atv_pair_show_popup(app, "Pairing", "Verifying PIN...");
            wlan_androidtv_op_pair_finish(app->androidtv, app->androidtv_pin);
            consumed = true;
        } else if(event.event == WlanAppCustomEventAndroidTvRetry) {
            atv_pair_start(app);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        WlanAtvState st = wlan_androidtv_state(app->androidtv);
        if(s_phase == AtvPairStarting) {
            if(st == WlanAtvStatePairShowPin) {
                s_phase = AtvPairPin;
                atv_pair_show_pin_input(app);
            } else if(st == WlanAtvStateError) {
                s_phase = AtvPairError;
                atv_pair_show_error(app, wlan_androidtv_error(app->androidtv));
            }
        } else if(s_phase == AtvPairVerifying) {
            if(st == WlanAtvStatePaired) {
                // Trusted now — back to the remote scene, which connects.
                scene_manager_previous_scene(app->scene_manager);
            } else if(st == WlanAtvStateError) {
                s_phase = AtvPairError;
                atv_pair_show_error(app, wlan_androidtv_error(app->androidtv));
            }
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        if(s_phase == AtvPairStarting || s_phase == AtvPairVerifying) {
            // Cancel the in-flight worker op, then leave the whole feature.
            wlan_androidtv_disconnect(app->androidtv);
        }
        // Leave the feature entirely (skip the remote scene, back to the scan).
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, WlanAppSceneAndroidTvScan);
        consumed = true;
    }
    return consumed;
}

void wlan_app_scene_androidtv_pair_on_exit(void* context) {
    WlanApp* app = context;
    text_input_reset(app->text_input);
    popup_reset(app->popup);
    widget_reset(app->widget);
}
