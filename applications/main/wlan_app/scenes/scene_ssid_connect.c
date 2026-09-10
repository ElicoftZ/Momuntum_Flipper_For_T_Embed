#include "../wlan_app.h"
#include <wlan_hal.h>
#include <wlan_passwords.h>
#include "../wlan_netcut.h"
#include <wifi.h>

typedef enum {
    SsidConnectStateAskPassword = 0,
    SsidConnectStateConnecting,
    SsidConnectStateConnected,
    SsidConnectStateFailed,
} SsidConnectState;

#define SSID_CONNECT_TIMEOUT_TICKS 160 // 160 * 250 ms = 40 s
#define SSID_CONNECT_POPUP_MS 1000

static uint16_t s_connect_ticks;
static char s_failure_text[64];

static void ssid_connect_set_state(WlanApp* app, SsidConnectState state);

static bool ssid_connect_reason_is_auth_failure(uint8_t reason) {
    return reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_802_1X_AUTH_FAILED || reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

static const char* ssid_connect_failure_text(char* buffer, size_t buffer_size) {
    const uint8_t reason = wlan_hal_get_last_disconnect_reason();
    const char* category = "Connection failed";
    if(reason == WIFI_REASON_NO_AP_FOUND) {
        category = "Network not found";
    } else if(reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
              reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD) {
        category = "Security not supported";
    } else if(reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD) {
        category = "Signal too weak";
    } else if(reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_802_1X_AUTH_FAILED) {
        /* AUTH_FAIL is also used for WPA3 negotiation failures, so calling it
         * a wrong password sends the user in the wrong direction. */
        category = "Authentication failed";
    } else if(reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
              reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
        category = "Security handshake failed";
    } else if(reason == WIFI_REASON_ASSOC_FAIL || reason == WIFI_REASON_ASSOC_NOT_AUTHED) {
        category = "Router rejected connection";
    } else if(reason == WIFI_REASON_BEACON_TIMEOUT) {
        category = "Network signal lost";
    }
    if(reason == 0) return "Connection timed out";

    snprintf(buffer, buffer_size, "%s\nCode %u", category, (unsigned)reason);
    return buffer;
}

static void ssid_connect_password_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventPasswordEntered);
}

static void ssid_connect_popup_cb(void* context) {
    WlanApp* app = context;
    SsidConnectState s = (SsidConnectState)scene_manager_get_scene_state(
        app->scene_manager, WlanAppSceneSsidConnect);

    if(s == SsidConnectStateConnected) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventConnectSuccess);
    } else if(s == SsidConnectStateFailed) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventConnectFailed);
    }
}

static void ssid_connect_show_popup(WlanApp* app, const char* text, uint32_t timeout_ms) {
    popup_reset(app->popup);
    popup_set_header(app->popup, "WiFi", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, text, 64, 32, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, ssid_connect_popup_cb);
    if(timeout_ms > 0) {
        popup_set_timeout(app->popup, timeout_ms);
        popup_enable_timeout(app->popup);
    } else {
        popup_disable_timeout(app->popup);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
}

static void ssid_connect_set_state(WlanApp* app, SsidConnectState state) {
    scene_manager_set_scene_state(app->scene_manager, WlanAppSceneSsidConnect, state);

    switch(state) {
    case SsidConnectStateAskPassword:
        text_input_reset(app->text_input);
        app->password_input[0] = '\0';
        text_input_set_header_text(app->text_input, "WiFi Password:");
        text_input_set_result_callback(
            app->text_input,
            ssid_connect_password_cb,
            app,
            app->password_input,
            sizeof(app->password_input),
            true);
        /* WiFi passphrases are case-sensitive.  The generic text editor's
         * sentence-style first-letter capitalization silently changed common
         * lowercase passwords and made correct input fail authentication. */
        text_input_set_auto_capitalize(app->text_input, false);
        view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewTextInput);
        break;

    case SsidConnectStateConnecting:
        s_connect_ticks = 0;
        if((!wlan_hal_is_started() && !wlan_hal_start()) ||
           !wlan_hal_connect(
               app->target_ap.ssid,
               app->password_input,
               app->target_ap.bssid,
               app->target_ap.channel)) {
            ssid_connect_set_state(app, SsidConnectStateFailed);
            break;
        }
        ssid_connect_show_popup(app, "Connecting ...", 0);
        break;

    case SsidConnectStateConnected:
        ssid_connect_show_popup(app, "Connected!", SSID_CONNECT_POPUP_MS);
        break;

    case SsidConnectStateFailed:
        ssid_connect_show_popup(
            app,
            ssid_connect_failure_text(s_failure_text, sizeof(s_failure_text)),
            SSID_CONNECT_POPUP_MS);
        break;
    }
}

void wlan_app_scene_ssid_connect_on_enter(void* context) {
    WlanApp* app = context;
    s_connect_ticks = 0;

    // Wenn ein Passwort bereits auf der SD existiert, lade es vor dem Connect.
    if(!app->target_ap.is_open && app->target_ap.has_password) {
        app->target_ap.has_password = wlan_password_read(
            app->target_ap.ssid, app->password_input, sizeof(app->password_input));
    } else if(app->target_ap.is_open) {
        app->password_input[0] = '\0';
    }

    bool need_password = !app->target_ap.is_open && !app->target_ap.has_password;
    ssid_connect_set_state(
        app, need_password ? SsidConnectStateAskPassword : SsidConnectStateConnecting);
}

bool wlan_app_scene_ssid_connect_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventPasswordEntered) {
            /* Keep a new password in RAM until association and DHCP succeed.
             * Saving before verification made a typo fail forever without
             * asking again on the next attempt. */
            ssid_connect_set_state(app, SsidConnectStateConnecting);
            consumed = true;
        } else if(event.event == WlanAppCustomEventConnectSuccess) {
            /* Background WiFi reconnects this network after the app (or an
             * Evil Portal session) releases the radio. Passwords remain in the
             * existing per-SSID files; this stores only the last SSID. */
            if(!app->target_ap.is_open && app->password_input[0]) {
                app->target_ap.has_password =
                    wlan_password_save(app->target_ap.ssid, app->password_input);
            }
            wlan_last_ssid_save(app->target_ap.ssid);
            memcpy(&app->connected_ap, &app->target_ap, sizeof(WlanApRecord));
            app->connected = true;
            app->target_selected = false;
            app->lan_scan_complete = false;
            /* WiFi global/„sticky" machen: nach App-Verlassen verbunden bleiben,
             * SSID für Auto-Reconnect merken, BLE bleibt aus. */
            Wifi* wifi = furi_record_open(RECORD_WIFI);
            wifi_mark_connected(wifi, app->connected_ap.ssid);
            furi_record_close(RECORD_WIFI);
            if(app->webfs_flow) {
                // Web-Filesystem over STA: serve on the fresh connection.
                scene_manager_set_scene_state(
                    app->scene_manager, WlanAppSceneWebFsInfo, 0 /* STA */);
                scene_manager_next_scene(app->scene_manager, WlanAppSceneWebFsInfo);
            } else if(app->fw_update_flow) {
                // Kombiniertes Update (FW + SD) → direkt zur Update-Scene.
                scene_manager_next_scene(app->scene_manager, WlanAppSceneFwUpdate);
            } else {
                // gw_mac aus DHCP-Antwort sollte jetzt in der ARP-Tabelle stehen.
                wlan_netcut_preflight(app->netcut);
                scene_manager_next_scene(
                    app->scene_manager, WlanAppSceneNetworkScanning);
            }
            consumed = true;
        } else if(event.event == WlanAppCustomEventConnectFailed) {

            if(!app->target_ap.is_open && wlan_hal_last_fail_is_auth()) {
                wlan_password_delete(app->target_ap.ssid);
                app->target_ap.has_password = false;

                if(app->ap_selected_index < app->ap_count) {
                    app->ap_records[app->ap_selected_index].has_password = false;
                }
            }
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        SsidConnectState s = (SsidConnectState)scene_manager_get_scene_state(
            app->scene_manager, WlanAppSceneSsidConnect);
        if(s == SsidConnectStateConnecting) {
            if(wlan_hal_is_connected()) {
                ssid_connect_set_state(app, SsidConnectStateConnected);
            } else if(++s_connect_ticks >= SSID_CONNECT_TIMEOUT_TICKS) {
                /* Do not block the application's event loop (and indirectly the
                 * global GUI input thread) while the driver cancels retries. */
                wlan_hal_disconnect_async();
                ssid_connect_set_state(app, SsidConnectStateFailed);
            }
        }
    }

    return consumed;
}

void wlan_app_scene_ssid_connect_on_exit(void* context) {
    WlanApp* app = context;
    popup_reset(app->popup);
    text_input_reset(app->text_input);
    s_connect_ticks = 0;
    scene_manager_set_scene_state(
        app->scene_manager, WlanAppSceneSsidConnect, SsidConnectStateAskPassword);
}
