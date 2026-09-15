#include "../marauder_app_i.h"

/* RGB888 -> RGB565 with the ESP32-S3 SPI byte swap. Same formula as
 * color_picker_pack_swap() in notification_settings_color_picker.c --
 * mirrored here rather than shared, since that one is file-local static. */
static inline uint16_t marauder_pack_swap(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
    return ((v & 0xFF) << 8) | (v >> 8);
}

/* Classic pentest-tool green. Applied only while this scene (Marauder's own
 * menu/about screens) is what's on screen -- see marauder_launch() and the
 * loader pubsub callback below for where it comes off and back on. */
#define MARAUDER_INK_R 0x00
#define MARAUDER_INK_G 0xFF
#define MARAUDER_INK_B 0x00

static void marauder_apply_ink(void) {
    furi_hal_display_set_fg_color(
        marauder_pack_swap(MARAUDER_INK_R, MARAUDER_INK_G, MARAUDER_INK_B));
}

/* Fires on every loader event; only LoaderEventTypeNoMoreAppsInQueue means
 * something -- the launched app-chain has fully unwound and the loader has
 * handed focus back to us. Same signal archive_scene_browser.c subscribes to
 * for its own "I'm back on top" refresh. */
static void marauder_loader_callback(const void* message, void* context) {
    furi_assert(context);
    const LoaderEvent* event = message;
    if(event->type == LoaderEventTypeNoMoreAppsInQueue) {
        marauder_apply_ink();
    }
}

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

    marauder_apply_ink();
    app->loader_stop_subscription =
        furi_pubsub_subscribe(loader_get_pubsub(app->loader), marauder_loader_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, MarauderAppViewSubmenu);
}

/* name/args as loader_start_with_gui_error expects -- name must match the
 * TARGET app's registered application.fam `name=`, not the label shown above.
 *
 * Restores the color BEFORE handing off so the launched app renders in the
 * user's own color, never Marauder's -- the tint is scoped to Marauder's own
 * screens only. marauder_loader_callback puts the tint back once the loader
 * reports we're back on top. */
static void marauder_launch(MarauderApp* app, const char* name, const char* args) {
    furi_hal_display_set_fg_color(app->saved_fg_color);
    loader_start_with_gui_error(app->loader, name, args);
}

bool marauder_scene_main_on_event(void* context, SceneManagerEvent event) {
    MarauderApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;
        switch(event.event) {
        case MarauderMainIndexWifiTools:
            marauder_launch(app, "WiFi", NULL);
            break;
        case MarauderMainIndexHandshake:
            marauder_launch(app, "WiFi", "handshake");
            break;
        case MarauderMainIndexSsidSpam:
            marauder_launch(app, "WiFi", "ssidspam");
            break;
        case MarauderMainIndexProbeFlood:
            marauder_launch(app, "WiFi", "probeflood");
            break;
        case MarauderMainIndexBleScan:
            marauder_launch(app, "BLE Detector", NULL);
            break;
        case MarauderMainIndexBleSpam:
            marauder_launch(app, "Bluetooth", NULL);
            break;
        case MarauderMainIndexWardriving:
            marauder_launch(app, "Wardriving", NULL);
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
    if(app->loader_stop_subscription) {
        furi_pubsub_unsubscribe(loader_get_pubsub(app->loader), app->loader_stop_subscription);
        app->loader_stop_subscription = NULL;
    }
    submenu_reset(app->submenu);
}
