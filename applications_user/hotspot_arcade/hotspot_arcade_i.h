#pragma once

#include "hotspot_arcade_config.h"
#include "scenes/hotspot_arcade_scene.h"

#include <furi.h>
#include <gui/gui.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <hotspot_arcade_service/hotspot_arcade_service.h>
#include <notification/notification_app.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#define HOTSPOT_ARCADE_WEB_STORAGE_PATH \
    EXT_PATH("apps_assets/hotspot_arcade/web/index.html.gz")
#define HOTSPOT_ARCADE_PACKS_STORAGE_DIR EXT_PATH("apps_assets/hotspot_arcade/packs")

/* These are handed to the firmware runtime, which opens them with the STORAGE
 * API (storage_file_open / storage_dir_open) -- not raw stdio. That API only
 * understands the "/ext" namespace; a literal "/sdcard/..." path is not mapped
 * and simply fails to open. They used to be spelled "/sdcard/...", which meant
 * the FAP's existence check (done on the /ext form, and passing) disagreed with
 * the open the runtime actually performed, and Start Session failed with a bare
 * "storage error" on a card that genuinely had the files. Keep these in the
 * /ext namespace to match the API that consumes them. */
/* ALL of these are now in the "/ext" namespace, and must stay there.
 *
 * Both consumers go through the Furi storage API: web_gzip_path via
 * storage_file_open() in the runtime, and the packs/art dirs via
 * storage_dir_exists()/storage_simply_mkdir() in path_is_directory and
 * ensure_directory. That API resolves "/ext" and maps it to the card itself.
 *
 * The packs/art dirs used to need "/sdcard/..." because those two helpers were
 * POSIX; they no longer are. Nothing is mounted behind /sdcard on this port, so
 * a /sdcard path now fails for every consumer. Getting a namespace wrong still
 * surfaces identically as a bare "storage error" at Start Session. */
#define HOTSPOT_ARCADE_WEB_SERVICE_PATH HOTSPOT_ARCADE_WEB_STORAGE_PATH
#define HOTSPOT_ARCADE_BUNDLED_PACKS_SERVICE_DIR HOTSPOT_ARCADE_PACKS_STORAGE_DIR
#define HOTSPOT_ARCADE_USER_PACKS_SERVICE_DIR EXT_PATH("apps_data/hotspot_arcade/packs")

#define HOTSPOT_ARCADE_CONSOLE_BUFFER_SIZE 4096U

typedef enum {
    HotspotArcadeViewSubmenu,
    HotspotArcadeViewTextInput,
    HotspotArcadeViewWidget,
} HotspotArcadeView;

typedef enum {
    HotspotArcadeEventSnapshotChanged = 0x1000U,
    HotspotArcadeEventTextInputDone,
    HotspotArcadeEventWidgetAction,
} HotspotArcadeEvent;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Storage* storage;
    NotificationApp* notifications;
    Submenu* submenu;
    TextInput* text_input;
    Widget* widget;

    HotspotArcadeSettings settings;
    HotspotArcadeServiceSnapshot snapshot;
    bool have_snapshot;

    char ssid_edit[HOTSPOT_ARCADE_SSID_MAX_LENGTH + 1U];
    char* console_buffer;
    FuriString* display_text;
    FuriString* message_title;
    FuriString* message_body;
} HotspotArcadeApp;

bool hotspot_arcade_refresh_snapshot(HotspotArcadeApp* app);
void hotspot_arcade_show_message(
    HotspotArcadeApp* app,
    const char* title,
    const char* body);
void hotspot_arcade_feedback(HotspotArcadeApp* app);
const char* hotspot_arcade_game_name(uint8_t game_id);
