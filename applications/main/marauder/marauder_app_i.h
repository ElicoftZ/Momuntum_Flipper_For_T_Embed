#pragma once

/* Marauder -- a single branded menu tree over this firmware's own WiFi/BLE
 * offensive-security apps, plus the couple of features (PMKID capture, probe-
 * request flood) that ESP32Marauder has and this port didn't. Nothing here
 * reimplements those apps: every leaf either launches one of them via the
 * loader's deferred-launch queue (loader_enqueue_launch() + exiting Marauder
 * itself -- see marauder_launch() in scenes/scene_main.c for why a direct
 * loader_start_with_gui_error() can't work here) or, for a few scenes
 * verified safe to enter with no prior state, deep-links straight past that
 * app's own main menu via a launch arg it already recognizes (wlan_app's
 * "handshake"/"ssidspam"/"probeflood").
 * See NOTICE for what in this codebase is credited to the real ESP32Marauder
 * project and why. */

#include "scenes/scene.h"

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/loading.h>
#include <loader/loader.h>
#include <furi_hal_display.h>

typedef struct {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* widget;
    /* Shown by marauder_launch() for a beat before handing off, so the user
     * sees an explicit "loading" transition instead of jumping straight from
     * Marauder's own menu into the launched app's -- every leaf goes through
     * marauder_launch(), so this covers all of them, not just WiFi. */
    Loading* loading;
    /* Whatever the user's own shell-color setting had the UI "ink" tinted to
     * before this app touched it. Restored before handing off to a launched
     * app (so WiFi/BLE Detector/etc. render in the user's own color, not
     * Marauder's) and one final time on exit -- the tint never outlives
     * Marauder's own screens. */
    uint16_t saved_fg_color;
    /* Held open for scene_main's pubsub subscription (see below); opening it
     * per-launch like the loader-invoking scenes elsewhere in this codebase
     * do would miss the "an app we launched just closed" event this needs. */
    Loader* loader;
    /* Set only while MarauderSceneMain is the active scene -- re-applies the
     * green tint exactly when the loader reports the launched app-chain has
     * fully unwound and focus is back with us (LoaderEventTypeNoMoreAppsInQueue),
     * the same signal archive_scene_browser.c uses to know it's back on top. */
    FuriPubSubSubscription* loader_stop_subscription;
} MarauderApp;

typedef enum {
    MarauderAppViewSubmenu,
    MarauderAppViewWidget,
    MarauderAppViewLoading,
} MarauderAppView;
