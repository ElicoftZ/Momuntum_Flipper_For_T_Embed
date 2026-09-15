#include "../wlan_app.h"
#include <wlan_hal.h>

// Probe-Request-Flood: spoofed-MAC probe requests for random SSIDs, target-
// unabhängig wie SSID Spam -- kein channel_mode nötig.

static void probe_flood_rebuild(WlanApp* app, uint32_t frames) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 8, AlignCenter, AlignTop, FontPrimary, "Probe Flood");

    char frames_buf[32];
    snprintf(frames_buf, sizeof(frames_buf), "Frames: %lu", (unsigned long)frames);
    widget_add_string_element(
        app->widget, 64, 30, AlignCenter, AlignTop, FontSecondary, frames_buf);

    const char* status =
        wlan_hal_probe_flood_is_running() ? "Flooding..." : "Stopped";
    widget_add_string_element(
        app->widget, 64, 46, AlignCenter, AlignTop, FontSecondary, status);
}

void wlan_app_scene_probe_flood_on_enter(void* context) {
    WlanApp* app = context;
    wlan_hal_probe_flood_start();
    probe_flood_rebuild(app, 0);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

bool wlan_app_scene_probe_flood_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        probe_flood_rebuild(app, wlan_hal_probe_flood_get_frame_count());
    }
    return consumed;
}

void wlan_app_scene_probe_flood_on_exit(void* context) {
    WlanApp* app = context;
    wlan_hal_probe_flood_stop();
    widget_reset(app->widget);
}
