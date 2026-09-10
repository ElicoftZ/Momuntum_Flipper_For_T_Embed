/* Android TV step 3: the remote control.
 *
 * Opens the long-lived remote-control session with the selected TV and shows
 * the button-grid remote view. If the TV doesn't trust our certificate yet the
 * connect returns NeedsPair and we route to the pairing scene; on return we
 * connect again. Key presses from the view are queued to the worker via
 * wlan_androidtv_send_key(). Back tears the session down and returns to the
 * scan. */

#include "../wlan_app.h"
#include "../wlan_androidtv.h"
#include "../views/wlan_androidtv_remote_view.h"

#include <string.h>
#include <stdio.h>

typedef enum {
    AtvRemoteConnecting = 0,
    AtvRemoteLive,
    AtvRemoteError,
} AtvRemotePhase;

static AtvRemotePhase s_phase;

static void atv_remote_key_cb(void* context, int keycode) {
    WlanApp* app = context;
    wlan_androidtv_send_key(app->androidtv, keycode);
}

static void atv_remote_show_popup(WlanApp* app, const char* header, const char* text) {
    popup_reset(app->popup);
    popup_set_header(app->popup, header, 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, text, 64, 34, AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
}

static void atv_remote_retry_button_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventAndroidTvRetry);
    }
}

static void atv_remote_show_error(WlanApp* app, const char* msg) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 12, AlignCenter, AlignTop, FontPrimary, "Android TV");
    widget_add_string_element(app->widget, 64, 34, AlignCenter, AlignCenter, FontSecondary, msg);
    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Retry", atv_remote_retry_button_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

static void atv_remote_connect(WlanApp* app) {
    if(!app->androidtv) app->androidtv = wlan_androidtv_alloc();
    if(!app->androidtv) {
        s_phase = AtvRemoteError;
        atv_remote_show_error(app, "Out of memory");
        return;
    }
    s_phase = AtvRemoteConnecting;
    atv_remote_show_popup(app, "Android TV", "Connecting...");
    wlan_androidtv_op_connect(app->androidtv, app->androidtv_ip);
}

void wlan_app_scene_androidtv_remote_on_enter(void* context) {
    WlanApp* app = context;
    wlan_androidtv_remote_view_set_key_callback(
        app->view_androidtv_remote, atv_remote_key_cb, app);
    atv_remote_connect(app);
}

bool wlan_app_scene_androidtv_remote_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventAndroidTvRetry) {
            atv_remote_connect(app);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        WlanAtvState st = wlan_androidtv_state(app->androidtv);
        if(s_phase == AtvRemoteConnecting) {
            if(st == WlanAtvStateConnected) {
                s_phase = AtvRemoteLive;
                char name[WLAN_ATV_NAME_MAX];
                wlan_androidtv_device_name(app->androidtv, name, sizeof(name));
                const char* title = name[0] ? name :
                    (app->androidtv_name[0] ? app->androidtv_name : "Android TV");
                wlan_androidtv_remote_view_set_status(app->view_androidtv_remote, title, true);
                view_dispatcher_switch_to_view(
                    app->view_dispatcher, WlanAppViewAndroidTvRemote);
            } else if(st == WlanAtvStateNeedsPair) {
                scene_manager_next_scene(app->scene_manager, WlanAppSceneAndroidTvPair);
            } else if(st == WlanAtvStateError) {
                s_phase = AtvRemoteError;
                atv_remote_show_error(app, wlan_androidtv_error(app->androidtv));
            }
        } else if(s_phase == AtvRemoteLive) {
            bool live = wlan_androidtv_is_connected(app->androidtv);
            wlan_androidtv_remote_view_set_status(app->view_androidtv_remote, NULL, live);
            if(!live) {
                s_phase = AtvRemoteError;
                const char* e = wlan_androidtv_error(app->androidtv);
                atv_remote_show_error(app, e[0] ? e : "Connection lost");
            }
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        wlan_androidtv_disconnect(app->androidtv);
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }
    return consumed;
}

void wlan_app_scene_androidtv_remote_on_exit(void* context) {
    WlanApp* app = context;
    // Ensure the session is down whenever we leave (Back, or routing to pair).
    if(app->androidtv && wlan_androidtv_is_connected(app->androidtv)) {
        wlan_androidtv_disconnect(app->androidtv);
    }
    popup_reset(app->popup);
    widget_reset(app->widget);
}
