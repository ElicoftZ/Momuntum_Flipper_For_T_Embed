#pragma once

/* Marauder -- a single branded menu tree over this firmware's own WiFi/BLE
 * offensive-security apps, plus the couple of features (PMKID capture, probe-
 * request flood) that ESP32Marauder has and this port didn't. Nothing here
 * reimplements those apps: every leaf either launches one of them via the
 * loader (loader_start_with_gui_error(), the same primitive the OS menu
 * itself uses) or, for a few scenes verified safe to enter with no prior
 * state, deep-links straight past that app's own main menu via a launch arg
 * it already recognizes (wlan_app's "handshake"/"ssidspam"/"probeflood").
 * See NOTICE for what in this codebase is credited to the real ESP32Marauder
 * project and why. */

#include "scenes/scene.h"

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <loader/loader.h>

typedef struct {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* widget;
} MarauderApp;

typedef enum {
    MarauderAppViewSubmenu,
    MarauderAppViewWidget,
} MarauderAppView;
