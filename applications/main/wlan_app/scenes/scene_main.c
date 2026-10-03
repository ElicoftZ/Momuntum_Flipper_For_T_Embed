#include "../wlan_app.h"
#include <wlan_hal.h>
#include <lwip/ip4_addr.h>
#include <storage/storage.h>
#include <furi_hal_rtc.h>
#include <stdio.h>
#include <stdlib.h>

enum MainIndex {
    MainIndexSelectWifi,
    MainIndexAttack,
    MainIndexSwitchWifi,
    MainIndexDisconnect,
    // Channel-Aktionen (immer sichtbar, unter dem Separator).
    MainIndexChannelHandshake = 10,
    MainIndexChannelDeauth = 11,
    MainIndexChannelSniffer = 12,
    MainIndexChannelSsidSpam = 13,
    MainIndexChannelSmartDeauth = 14,
    MainIndexChannelEvilPortal = 15,
    MainIndexConnectionDetails = 16,
    MainIndexWebFs = 17,
    MainIndexProbeSniff = 18,
    MainIndexUpdateSourceSettings = 19,
    MainIndexProbeFlood = 20,
    MainIndexChannelSurvey = 21,
    MainIndexSaveChannelSurvey = 22,
    MainIndexWifiStatus = 23,
    MainIndexRssiHistory = 24,
};

static uint16_t s_channel_counts[14];
static uint16_t s_survey_total;

#define WLAN_RSSI_HISTORY_SAMPLES 30
static struct {
    bool active;
    uint8_t count;
    uint32_t last_sample_tick;
    DateTime started_at;
    int8_t rssi[WLAN_RSSI_HISTORY_SAMPLES];
    uint8_t channel[WLAN_RSSI_HISTORY_SAMPLES];
} s_rssi_history;

static void wlan_app_scene_main_submenu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void wlan_channel_survey_save_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MainIndexSaveChannelSurvey);
    }
}

static void wlan_channel_survey_show(WlanApp* app, const char* message) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 10, AlignCenter, AlignBottom, FontPrimary, "WiFi channel survey");
    widget_add_text_box_element(app->widget, 0, 14, 128, 47, AlignLeft, AlignTop, message, false);
    if(s_survey_total) {
        widget_add_button_element(
            app->widget,
            GuiButtonTypeCenter,
            "Save CSV",
            wlan_channel_survey_save_cb,
            app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

static void wlan_channel_survey_run(WlanApp* app) {
    memset(s_channel_counts, 0, sizeof(s_channel_counts));
    s_survey_total = 0;
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLoading);

    if(!wlan_hal_is_started() && !wlan_hal_start()) {
        wlan_channel_survey_show(app, "WiFi could not start");
        return;
    }

    wifi_ap_record_t* records = NULL;
    uint16_t count = 0;
    if(!wlan_hal_scan(&records, &count, WLAN_APP_MAX_APS)) {
        if(records) free(records);
        wlan_channel_survey_show(app, "Scan failed");
        return;
    }

    for(uint16_t i = 0; i < count; ++i) {
        const uint8_t channel = records[i].primary;
        if(channel >= 1 && channel <= 13) s_channel_counts[channel]++;
    }
    s_survey_total = count;
    if(records) free(records);

    char message[144];
    snprintf(
        message,
        sizeof(message),
        "%u networks\n1:%u  2:%u  3:%u  4:%u\n5:%u  6:%u  7:%u  8:%u\n9:%u 10:%u 11:%u\n12:%u 13:%u",
        (unsigned)s_survey_total,
        (unsigned)s_channel_counts[1],
        (unsigned)s_channel_counts[2],
        (unsigned)s_channel_counts[3],
        (unsigned)s_channel_counts[4],
        (unsigned)s_channel_counts[5],
        (unsigned)s_channel_counts[6],
        (unsigned)s_channel_counts[7],
        (unsigned)s_channel_counts[8],
        (unsigned)s_channel_counts[9],
        (unsigned)s_channel_counts[10],
        (unsigned)s_channel_counts[11],
        (unsigned)s_channel_counts[12],
        (unsigned)s_channel_counts[13]);
    wlan_channel_survey_show(app, message);
}

static void wlan_channel_survey_save(WlanApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, EXT_PATH("wifi"));
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(
        file,
        EXT_PATH("wifi/channel_survey.csv"),
        FSAM_WRITE,
        FSOM_CREATE_ALWAYS);
    if(ok) {
        static const char header[] = "channel,network_count\n";
        ok = storage_file_write(file, header, sizeof(header) - 1) == sizeof(header) - 1;
        for(uint8_t channel = 1; ok && channel <= 13; ++channel) {
            char row[16];
            int length = snprintf(
                row,
                sizeof(row),
                "%u,%u\n",
                (unsigned)channel,
                (unsigned)s_channel_counts[channel]);
            ok = length > 0 && storage_file_write(file, row, (size_t)length) == (size_t)length;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    wlan_channel_survey_show(
        app,
        ok ? "Saved to /ext/wifi/channel_survey.csv" : "Could not save CSV");
}

static bool wlan_rssi_history_save(void) {
    if(s_rssi_history.count == 0) return true;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, EXT_PATH("wifi"));
    const char* path = EXT_PATH("wifi/rssi_history.csv");
    FileInfo info;
    const bool needs_header = storage_common_stat(storage, path, &info) != FSE_OK || info.size == 0;
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, path, FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ok && needs_header) {
        static const char header[] = "started,elapsed_seconds,rssi_dbm,channel\n";
        ok = storage_file_write(file, header, sizeof(header) - 1) == sizeof(header) - 1;
    }
    char started[32];
    snprintf(
        started,
        sizeof(started),
        "%04u-%02u-%02uT%02u:%02u:%02u",
        (unsigned)s_rssi_history.started_at.year,
        (unsigned)s_rssi_history.started_at.month,
        (unsigned)s_rssi_history.started_at.day,
        (unsigned)s_rssi_history.started_at.hour,
        (unsigned)s_rssi_history.started_at.minute,
        (unsigned)s_rssi_history.started_at.second);
    for(uint8_t i = 0; ok && i < s_rssi_history.count; ++i) {
        char row[64];
        int length = snprintf(
            row,
            sizeof(row),
            "%s,%u,%d,%u\n",
            started,
            (unsigned)i,
            (int)s_rssi_history.rssi[i],
            (unsigned)s_rssi_history.channel[i]);
        ok = length > 0 && storage_file_write(file, row, (size_t)length) == (size_t)length;
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static void wlan_rssi_history_show(WlanApp* app, const char* result) {
    char message[96];
    if(result) {
        snprintf(message, sizeof(message), "%s\n%u samples", result, (unsigned)s_rssi_history.count);
    } else {
        snprintf(
            message,
            sizeof(message),
            "Recording RSSI\nSample %u/%u\nBack saves partial",
            (unsigned)s_rssi_history.count,
            WLAN_RSSI_HISTORY_SAMPLES);
    }
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 10, AlignCenter, AlignBottom, FontPrimary, "WiFi signal history");
    widget_add_text_box_element(app->widget, 0, 14, 128, 48, AlignLeft, AlignTop, message, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

static void wlan_rssi_history_stop(WlanApp* app) {
    s_rssi_history.active = false;
    wlan_rssi_history_show(
        app,
        wlan_rssi_history_save() ? "Saved /ext/wifi/rssi_history.csv" : "Could not save CSV");
}

void wlan_app_scene_main_on_enter(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, "WiFi");

    // Channel-Aktionen sind immer sichtbar; Verbindungs-Aktionen sind state-abhängig.
    app->channel_mode_active = false;
    // Hub-Scene: ein evtl. abgebrochener Update-SD-Flow wird hier zurückgesetzt.
    app->webfs_flow = false;
    app->fw_update_flow = false;

    if(!app->connected && !app->target_selected) {
        submenu_add_item_centered(
            app->submenu, "Select Wifi", MainIndexSelectWifi, wlan_app_scene_main_submenu_cb, app);
    } else if(app->connected) {
        char label[48];
        snprintf(label, sizeof(label), "%s Attack", app->connected_ap.ssid);
        submenu_add_item(
            app->submenu, label, MainIndexAttack, wlan_app_scene_main_submenu_cb, app);
        submenu_add_item(
            app->submenu, "Switch Wifi", MainIndexSwitchWifi, wlan_app_scene_main_submenu_cb, app);
        submenu_add_item(
            app->submenu, "Disconnect", MainIndexDisconnect, wlan_app_scene_main_submenu_cb, app);
        submenu_add_item(
            app->submenu,
            "Connection Details",
            MainIndexConnectionDetails,
            wlan_app_scene_main_submenu_cb,
            app);
    } else {
        char label[48];
        snprintf(label, sizeof(label), "%s Attack", app->target_ap.ssid);
        submenu_add_item(
            app->submenu, label, MainIndexAttack, wlan_app_scene_main_submenu_cb, app);
        submenu_add_item(
            app->submenu, "Switch Wifi", MainIndexSwitchWifi, wlan_app_scene_main_submenu_cb, app);
    }

    submenu_add_separator(app->submenu);

    submenu_add_item(
        app->submenu, "Capture Handshake", MainIndexChannelHandshake,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Deauth", MainIndexChannelDeauth,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Sniffer", MainIndexChannelSniffer,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Probe Sniff", MainIndexProbeSniff,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Probe Flood", MainIndexProbeFlood,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "SSID Spam", MainIndexChannelSsidSpam,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Smart Deauth", MainIndexChannelSmartDeauth,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Evil Portal", MainIndexChannelEvilPortal,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Web-Filesystem", MainIndexWebFs,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Update Source", MainIndexUpdateSourceSettings,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu,
        "Channel Survey",
        MainIndexChannelSurvey,
        wlan_app_scene_main_submenu_cb,
        app);
    submenu_add_item(
        app->submenu, "WiFi Status", MainIndexWifiStatus, wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "RSSI History", MainIndexRssiHistory, wlan_app_scene_main_submenu_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_main_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick && s_rssi_history.active) {
        const uint32_t now = furi_get_tick();
        if((now - s_rssi_history.last_sample_tick) >= furi_ms_to_ticks(1000)) {
            s_rssi_history.last_sample_tick = now;
            wifi_ap_record_t ap = {0};
            if(!wlan_hal_get_connected_ap(&ap)) {
                wlan_rssi_history_stop(app);
            } else {
                const uint8_t index = s_rssi_history.count++;
                s_rssi_history.rssi[index] = ap.rssi;
                s_rssi_history.channel[index] = ap.primary;
                if(s_rssi_history.count >= WLAN_RSSI_HISTORY_SAMPLES) {
                    wlan_rssi_history_stop(app);
                } else {
                    wlan_rssi_history_show(app, NULL);
                }
            }
        }
        return true;
    }

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case MainIndexSelectWifi:
        case MainIndexSwitchWifi:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneConnect);
            consumed = true;
            break;
        case MainIndexAttack:
            if(app->connected) {
                // Erster Aufruf nach Connect/Disconnect: ARP-Scan; danach
                // direkt zur LAN-Liste, bis Re-Scan getriggert wird.
                if(app->lan_scan_complete) {
                    scene_manager_next_scene(app->scene_manager, WlanAppSceneLan);
                } else {
                    scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkScanning);
                }
            } else {
                scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkActions);
            }
            consumed = true;
            break;
        case MainIndexDisconnect:
            wlan_hal_disconnect();
            app->connected = false;
            app->target_selected = false;
            app->lan_scan_complete = false;
            memset(&app->connected_ap, 0, sizeof(app->connected_ap));
            wlan_app_scene_main_on_exit(app);
            wlan_app_scene_main_on_enter(app);
            consumed = true;
            break;
        case MainIndexChannelSurvey:
            wlan_channel_survey_run(app);
            consumed = true;
            break;
        case MainIndexWifiStatus: {
            char message[128];
            snprintf(
                message,
                sizeof(message),
                "Setting: %s\nRadio: %s\nLink: %s\nOTA hold: %s\nLast drop: %u",
                wlan_hal_is_user_enabled() ? "On" : "Off",
                wlan_hal_is_started() ? "Running" : "Stopped",
                wlan_hal_is_connected() ? "Connected" : "Disconnected",
                wlan_hal_is_held_after_update() ? "Yes" : "No",
                (unsigned)wlan_hal_get_last_disconnect_reason());
            widget_reset(app->widget);
            widget_add_string_element(
                app->widget, 64, 10, AlignCenter, AlignBottom, FontPrimary, "WiFi status");
            widget_add_text_box_element(
                app->widget, 0, 14, 128, 48, AlignLeft, AlignTop, message, false);
            view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
            consumed = true;
            break;
        }
        case MainIndexRssiHistory:
            memset(&s_rssi_history, 0, sizeof(s_rssi_history));
            if(!wlan_hal_is_connected() ||
               !wlan_hal_get_connected_ap(&(wifi_ap_record_t){0})) {
                wlan_rssi_history_show(app, "Connect to WiFi first");
            } else {
                furi_hal_rtc_get_datetime(&s_rssi_history.started_at);
                s_rssi_history.active = true;
                s_rssi_history.last_sample_tick = furi_get_tick();
                wlan_rssi_history_show(app, NULL);
            }
            consumed = true;
            break;
        case MainIndexSaveChannelSurvey:
            wlan_channel_survey_save(app);
            consumed = true;
            break;
        case MainIndexConnectionDetails: {
            wifi_ap_record_t ap = {0};
            char ip[16] = "Unavailable";
            char gateway[16] = "Unavailable";
            char dns[16] = "Unavailable";
            const uint32_t ip_addr = wlan_hal_get_own_ip();
            const uint32_t gateway_addr = wlan_hal_get_gw_ip();
            const uint32_t dns_addr = wlan_hal_get_dns_ip();
            if(ip_addr) ip4addr_ntoa_r((const ip4_addr_t*)&ip_addr, ip, sizeof(ip));
            if(gateway_addr)
                ip4addr_ntoa_r((const ip4_addr_t*)&gateway_addr, gateway, sizeof(gateway));
            if(dns_addr) ip4addr_ntoa_r((const ip4_addr_t*)&dns_addr, dns, sizeof(dns));

            widget_reset(app->widget);
            widget_add_string_element(
                app->widget, 64, 10, AlignCenter, AlignBottom, FontPrimary, "WiFi link");
            if(wlan_hal_get_connected_ap(&ap)) {
                char details[128];
                snprintf(
                    details,
                    sizeof(details),
                    "%.20s\nRSSI %d dBm  Ch %u\nIP %s\nGW %s\nDNS %s",
                    (const char*)ap.ssid,
                    ap.rssi,
                    (unsigned)ap.primary,
                    ip,
                    gateway,
                    dns);
                widget_add_text_box_element(
                    app->widget, 0, 14, 128, 48, AlignLeft, AlignTop, details, false);
            } else {
                widget_add_string_element(
                    app->widget, 64, 36, AlignCenter, AlignCenter, FontSecondary, "Not connected");
            }
            view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
            consumed = true;
            break;
        }
        case MainIndexChannelHandshake:
            app->channel_mode_active = true;
            if(app->channel_action_channel == 0) app->channel_action_channel = 1;
            scene_manager_next_scene(app->scene_manager, WlanAppSceneHandshake);
            consumed = true;
            break;
        case MainIndexChannelDeauth:
            app->channel_mode_active = true;
            if(app->channel_action_channel == 0) app->channel_action_channel = 1;
            scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkDeauth);
            consumed = true;
            break;
        case MainIndexProbeSniff:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneProbeSniff);
            consumed = true;
            break;
        case MainIndexProbeFlood:
            // Target-unabhängig wie SSID Spam → kein channel_mode.
            scene_manager_next_scene(app->scene_manager, WlanAppSceneProbeFlood);
            consumed = true;
            break;
        case MainIndexChannelSniffer:
            app->channel_mode_active = true;
            if(app->channel_action_channel == 0) app->channel_action_channel = 1;
            scene_manager_next_scene(app->scene_manager, WlanAppScenePackageSniffer);
            consumed = true;
            break;
        case MainIndexChannelSsidSpam:
            // SSID Spam ist target-unabhängig (reine Beacon-Frames) → kein channel_mode.
            scene_manager_next_scene(app->scene_manager, WlanAppSceneSsidSpam);
            consumed = true;
            break;
        case MainIndexChannelSmartDeauth:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneSmartDeauth);
            consumed = true;
            break;
        case MainIndexChannelEvilPortal:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneEvilPortalMenu);
            consumed = true;
            break;
        case MainIndexUpdateSourceSettings:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneUpdateSourceSettings);
            consumed = true;
            break;
        case MainIndexWebFs:
            // Already connected → straight to the info scene (serve over STA);
            // otherwise show the Select Wifi / Dedicated AP menu.
            if(app->connected) {
                scene_manager_set_scene_state(
                    app->scene_manager, WlanAppSceneWebFsInfo, 0 /* STA */);
                scene_manager_next_scene(app->scene_manager, WlanAppSceneWebFsInfo);
            } else {
                scene_manager_next_scene(app->scene_manager, WlanAppSceneWebFsMenu);
            }
            consumed = true;
            break;
        }
    }

    return consumed;
}

void wlan_app_scene_main_on_exit(void* context) {
    WlanApp* app = context;
    if(s_rssi_history.active) {
        s_rssi_history.active = false;
        wlan_rssi_history_save();
    }
    submenu_reset(app->submenu);
}
