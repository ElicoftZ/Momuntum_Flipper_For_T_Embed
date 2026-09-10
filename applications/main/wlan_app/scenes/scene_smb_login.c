/* SMB Browser step 2: username/password entry, then connect.
 *
 * Two TextInput fields driven by an internal phase (user -> password), then a
 * "Connecting..." popup while the worker validates the credentials against the
 * server's IPC$ share. On success we advance to the browser (share list); on
 * failure the popup shows the error and Back returns to the server list.
 *
 * Empty username + empty password attempts an anonymous/guest login. */

#include "../wlan_app.h"

#include <string.h>
#include <stdio.h>

// Path/label joins use snprintf into fixed buffers; truncation is safe
// (always NUL-terminated) and intentional for over-long inputs.
#pragma GCC diagnostic ignored "-Wformat-truncation"

typedef enum {
    SmbLoginPhaseUser = 0,
    SmbLoginPhasePass,
    SmbLoginPhaseConnecting,
    SmbLoginPhaseError,
} SmbLoginPhase;

static SmbLoginPhase s_phase;

static void smb_login_input_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventSmbInputDone);
}

static void smb_login_show_user(WlanApp* app) {
    TextInput* ti = app->text_input;
    text_input_reset(ti);
    text_input_set_validator(ti, NULL, NULL);
    text_input_set_header_text(ti, "User (empty = guest)");
    text_input_set_minimum_length(ti, 0);
    text_input_set_result_callback(
        ti, smb_login_input_cb, app, app->smb_user, WLAN_SMB_USER_MAX, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewTextInput);
}

static void smb_login_show_pass(WlanApp* app) {
    TextInput* ti = app->text_input;
    text_input_reset(ti);
    text_input_set_validator(ti, NULL, NULL);
    text_input_set_header_text(ti, "Password (empty = none)");
    text_input_set_minimum_length(ti, 0);
    text_input_set_result_callback(
        ti, smb_login_input_cb, app, app->smb_pass, WLAN_SMB_PASS_MAX, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewTextInput);
}

static void smb_login_show_connecting(WlanApp* app) {
    char text[64];
    snprintf(text, sizeof(text), "%s\nas %s", app->smb_server_ip,
             app->smb_user[0] ? app->smb_user : "guest");
    popup_reset(app->popup);
    popup_set_header(app->popup, "Connecting...", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, text, 64, 34, AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
}

static void smb_login_retry_button_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventSmbRetry);
    }
}

static void smb_login_show_error(WlanApp* app) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 14, AlignCenter, AlignTop, FontPrimary, "Login failed");
    widget_add_string_element(
        app->widget, 64, 34, AlignCenter, AlignCenter, FontSecondary, "Wrong user or password");
    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Retry", smb_login_retry_button_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

void wlan_app_scene_smb_login_on_enter(void* context) {
    WlanApp* app = context;
    s_phase = SmbLoginPhaseUser;
    smb_login_show_user(app);
}

bool wlan_app_scene_smb_login_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventSmbInputDone) {
            if(s_phase == SmbLoginPhaseUser) {
                s_phase = SmbLoginPhasePass;
                smb_login_show_pass(app);
            } else if(s_phase == SmbLoginPhasePass) {
                if(!app->smb) app->smb = wlan_smb_alloc();
                if(!app->smb) {
                    s_phase = SmbLoginPhaseError;
                    popup_reset(app->popup);
                    popup_set_header(app->popup, "Error", 64, 10, AlignCenter, AlignTop);
                    popup_set_text(
                        app->popup, "Out of memory", 64, 34, AlignCenter, AlignCenter);
                    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
                } else {
                    s_phase = SmbLoginPhaseConnecting;
                    smb_login_show_connecting(app);
                    wlan_smb_op_connect(
                        app->smb, app->smb_server_ip, app->smb_user, app->smb_pass);
                }
            }
            consumed = true;
        } else if(event.event == WlanAppCustomEventSmbRetry) {
            s_phase = SmbLoginPhaseUser;
            smb_login_show_user(app);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(s_phase == SmbLoginPhaseConnecting) {
            WlanSmbState st = wlan_smb_state(app->smb);
            if(st == WlanSmbStateReady) {
                // Logged in. Enter the browser at the share-list level.
                app->smb_share[0] = '\0';
                app->smb_path[0] = '\0';
                scene_manager_next_scene(app->scene_manager, WlanAppSceneSmbBrowser);
            } else if(st == WlanSmbStateError) {
                s_phase = SmbLoginPhaseError;
                smb_login_show_error(app);
            }
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        if(s_phase == SmbLoginPhaseConnecting) {
            // Ignore Back while the (bounded, ~8s) connect is in flight.
            consumed = true;
        }
        // Error/entry phases: let the scene manager pop back to the server
        // list (the on-screen "Retry" button handles retrying).
    }
    return consumed;
}

void wlan_app_scene_smb_login_on_exit(void* context) {
    WlanApp* app = context;
    text_input_reset(app->text_input);
    popup_reset(app->popup);
    widget_reset(app->widget);
}
