#include "../marauder_app_i.h"

/* Every leaf either launches an existing app (the loader's own
 * loader_start_with_gui_error(), same as tapping it from the OS main menu)
 * or, marked "deep link" below, jumps straight past that app's own main menu
 * via a launch arg it already recognizes -- verified safe to enter with no
 * prior state (see wlan_app.c's arg handling in its entry point). */

typedef enum {
    MarauderMainIndexWifiTools, /* launches "WiFi" -- its own menu covers
                                   Connect/Attack/Deauth/Sniffer/Evil Portal/
                                   SSID Spam/Smart Deauth/Probe Sniff+Flood */
    MarauderMainIndexHandshake, /* deep link: WiFi app, arg "handshake" */
    MarauderMainIndexSsidSpam, /* deep link: WiFi app, arg "ssidspam" */
    MarauderMainIndexProbeFlood, /* deep link: WiFi app, arg "probeflood" */
    MarauderMainIndexBleScan, /* launches "BLE Detector" */
    MarauderMainIndexBleSpam, /* launches "Bluetooth" (this port's BLE Spam
                                  app's registered menu name) */
    MarauderMainIndexWardriving, /* launches "Wardriving" */
    MarauderMainIndexAbout,
} MarauderMainIndex;

static void marauder_scene_main_submenu_cb(void* context, uint32_t index) {
    MarauderApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void marauder_scene_main_on_enter(void* context) {
    MarauderApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, "Marauder");

    submenu_add_item(
        app->submenu, "WiFi Tools", MarauderMainIndexWifiTools,
        marauder_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Capture Handshake+PMKID", MarauderMainIndexHandshake,
        marauder_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Beacon/SSID Spam", MarauderMainIndexSsidSpam,
        marauder_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Probe Request Flood", MarauderMainIndexProbeFlood,
        marauder_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "BLE Scanner", MarauderMainIndexBleScan,
        marauder_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "BLE Spam", MarauderMainIndexBleSpam,
        marauder_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Wardriving", MarauderMainIndexWardriving,
        marauder_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "About / Credits", MarauderMainIndexAbout,
        marauder_scene_main_submenu_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, MarauderAppViewSubmenu);
}

/* name/args as loader_start_with_gui_error expects -- name must match the
 * TARGET app's registered application.fam `name=`, not the label shown above. */
static void marauder_launch(const char* name, const char* args) {
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start_with_gui_error(loader, name, args);
    furi_record_close(RECORD_LOADER);
}

bool marauder_scene_main_on_event(void* context, SceneManagerEvent event) {
    MarauderApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;
        switch(event.event) {
        case MarauderMainIndexWifiTools:
            marauder_launch("WiFi", NULL);
            break;
        case MarauderMainIndexHandshake:
            marauder_launch("WiFi", "handshake");
            break;
        case MarauderMainIndexSsidSpam:
            marauder_launch("WiFi", "ssidspam");
            break;
        case MarauderMainIndexProbeFlood:
            marauder_launch("WiFi", "probeflood");
            break;
        case MarauderMainIndexBleScan:
            marauder_launch("BLE Detector", NULL);
            break;
        case MarauderMainIndexBleSpam:
            marauder_launch("Bluetooth", NULL);
            break;
        case MarauderMainIndexWardriving:
            marauder_launch("Wardriving", NULL);
            break;
        case MarauderMainIndexAbout:
            scene_manager_next_scene(app->scene_manager, MarauderSceneAbout);
            break;
        default:
            consumed = false;
            break;
        }
    }

    return consumed;
}

void marauder_scene_main_on_exit(void* context) {
    MarauderApp* app = context;
    submenu_reset(app->submenu);
}
