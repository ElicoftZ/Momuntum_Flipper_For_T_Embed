/* SMB Browser step 3: browse the remote filesystem.
 *
 * The shared LAN list view is reused. Navigation levels:
 *   - share list  (app->smb_share == "")  : entries are the server's shares
 *   - inside share (app->smb_share != "")  : entries are dirs/files at
 *                                            app->smb_path ("" = share root)
 *
 * Every level is loaded with wlan_smb_op_list(share, path); the empty share
 * makes the worker enumerate shares, so navigation is uniform. Short-OK
 * descends (or, on "..", ascends). Long-OK opens a one-item "Download" menu
 * that jumps to the download scene for the selected file/folder. */

#include "../wlan_app.h"

#include <assets_icons.h>
#include <string.h>
#include <stdio.h>

// Path joins use snprintf into fixed buffers; truncation is safe (always
// NUL-terminated) and intentional for over-long remote paths.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define SMB_BROWSER_UP_ID 0xFFFF

static bool s_loading;

static void smb_human_size(uint64_t bytes, char* out, size_t sz) {
    if(bytes >= 1024ULL * 1024 * 1024) {
        snprintf(out, sz, "%luG", (unsigned long)(bytes / (1024ULL * 1024 * 1024)));
    } else if(bytes >= 1024 * 1024) {
        snprintf(out, sz, "%luM", (unsigned long)(bytes / (1024 * 1024)));
    } else if(bytes >= 1024) {
        snprintf(out, sz, "%luK", (unsigned long)(bytes / 1024));
    } else {
        snprintf(out, sz, "%luB", (unsigned long)bytes);
    }
}

// Cut off the last '/'-separated component of a path in place (parent dir).
static void smb_path_parent(char* path) {
    char* slash = strrchr(path, '/');
    if(slash)
        *slash = '\0';
    else
        path[0] = '\0';
}

// Append a child component onto app->smb_path.
static void smb_path_push(WlanApp* app, const char* child) {
    size_t len = strlen(app->smb_path);
    if(len == 0) {
        strncpy(app->smb_path, child, WLAN_SMB_PATH_MAX - 1);
        app->smb_path[WLAN_SMB_PATH_MAX - 1] = '\0';
    } else {
        snprintf(
            app->smb_path + len, WLAN_SMB_PATH_MAX - len, "/%s", child);
    }
}

static void smb_browser_set_header(WlanApp* app) {
    const char* label;
    if(app->smb_share[0] == '\0') {
        label = app->smb_server_name[0] ? app->smb_server_name : app->smb_server_ip;
    } else if(app->smb_path[0] == '\0') {
        label = app->smb_share;
    } else {
        const char* slash = strrchr(app->smb_path, '/');
        label = slash ? slash + 1 : app->smb_path;
    }
    wlan_lan_view_set_header_title(app->view_lan, label);
}

static void smb_browser_load(WlanApp* app) {
    View* v = app->view_lan;
    wlan_lan_view_clear_menu(v);
    wlan_lan_view_close_menu(v);
    wlan_lan_view_clear(v);
    wlan_lan_view_set_empty_text(v, "Loading...");
    smb_browser_set_header(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLan);

    s_loading = true;
    wlan_smb_op_list(app->smb, app->smb_share, app->smb_path);
}

static void smb_browser_fill(WlanApp* app) {
    View* v = app->view_lan;
    wlan_lan_view_clear(v);
    wlan_lan_view_set_empty_text(v, "(empty)");

    // ".." entry whenever we're not at the top (share list root).
    if(app->smb_share[0] != '\0') {
        wlan_lan_view_add_action(v, "..", SMB_BROWSER_UP_ID);
    }

    uint16_t n = wlan_smb_entry_count(app->smb);
    for(uint16_t i = 0; i < n; ++i) {
        WlanSmbEntry e;
        if(!wlan_smb_entry(app->smb, i, &e)) continue;
        if(e.is_dir) {
            // Folder: folder icon, no size.
            wlan_lan_view_add_device_icon(
                v, e.name, NULL, NULL, NULL, &I_dir_10px, true, i);
        } else {
            // File: file icon + human-readable size on the right.
            char val[WLAN_LAN_VIEW_VALUE_MAX];
            smb_human_size(e.size, val, sizeof(val));
            wlan_lan_view_add_device_icon(
                v, e.name, NULL, val, NULL, &I_file_10px, true, i);
        }
    }
    smb_browser_set_header(app);
    wlan_lan_view_set_selected(v, 0);
}

// Fill app->smb_dl_* from the currently selected entry. Returns false for the
// ".." row (nothing to download).
static bool smb_browser_capture_target(WlanApp* app) {
    uint8_t sel = wlan_lan_view_get_selected(app->view_lan);
    WlanLanItem it = wlan_lan_view_get_item(app->view_lan, sel);
    if(it.user_id == SMB_BROWSER_UP_ID) return false;

    WlanSmbEntry e;
    if(!wlan_smb_entry(app->smb, it.user_id, &e)) return false;

    strncpy(app->smb_dl_name, e.name, sizeof(app->smb_dl_name) - 1);
    app->smb_dl_name[sizeof(app->smb_dl_name) - 1] = '\0';

    if(app->smb_share[0] == '\0') {
        // Share-list level: download the whole share.
        strncpy(app->smb_dl_share, e.name, sizeof(app->smb_dl_share) - 1);
        app->smb_dl_share[sizeof(app->smb_dl_share) - 1] = '\0';
        app->smb_dl_path[0] = '\0';
        app->smb_dl_is_dir = true;
    } else {
        strncpy(app->smb_dl_share, app->smb_share, sizeof(app->smb_dl_share) - 1);
        app->smb_dl_share[sizeof(app->smb_dl_share) - 1] = '\0';
        if(app->smb_path[0] == '\0') {
            strncpy(app->smb_dl_path, e.name, sizeof(app->smb_dl_path) - 1);
            app->smb_dl_path[sizeof(app->smb_dl_path) - 1] = '\0';
        } else {
            snprintf(
                app->smb_dl_path, sizeof(app->smb_dl_path), "%s/%s", app->smb_path, e.name);
        }
        app->smb_dl_is_dir = e.is_dir;
    }
    return true;
}

// Short-OK: descend into the selected share/dir, or go up on "..".
static void smb_browser_navigate(WlanApp* app) {
    uint8_t sel = wlan_lan_view_get_selected(app->view_lan);
    WlanLanItem it = wlan_lan_view_get_item(app->view_lan, sel);

    if(it.user_id == SMB_BROWSER_UP_ID) {
        if(app->smb_path[0] != '\0') {
            smb_path_parent(app->smb_path);
        } else {
            app->smb_share[0] = '\0'; // back to share list
        }
        smb_browser_load(app);
        return;
    }

    WlanSmbEntry e;
    if(!wlan_smb_entry(app->smb, it.user_id, &e)) return;

    if(app->smb_share[0] == '\0') {
        // Enter a share.
        strncpy(app->smb_share, e.name, sizeof(app->smb_share) - 1);
        app->smb_share[sizeof(app->smb_share) - 1] = '\0';
        app->smb_path[0] = '\0';
        smb_browser_load(app);
    } else if(e.is_dir) {
        smb_path_push(app, e.name);
        smb_browser_load(app);
    }
    // Files on short-OK: no action (long-OK offers download).
}

void wlan_app_scene_smb_browser_on_enter(void* context) {
    WlanApp* app = context;
    smb_browser_load(app);
}

bool wlan_app_scene_smb_browser_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        if(s_loading) {
            WlanSmbState st = wlan_smb_state(app->smb);
            if(st == WlanSmbStateReady) {
                s_loading = false;
                smb_browser_fill(app);
            } else if(st == WlanSmbStateError) {
                s_loading = false;
                wlan_lan_view_clear(app->view_lan);
                wlan_lan_view_set_empty_text(app->view_lan, wlan_smb_error(app->smb));
                smb_browser_set_header(app);
            }
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        if(s_loading) return true; // ignore input while a listing is in flight
        switch(event.event) {
        case WlanAppCustomEventLanItemOk:
            smb_browser_navigate(app);
            consumed = true;
            break;
        case WlanAppCustomEventLanItemLongOk:
            if(smb_browser_capture_target(app)) {
                wlan_lan_view_clear_menu(app->view_lan);
                wlan_lan_view_add_menu_item(app->view_lan, "Download", 0);
                wlan_lan_view_open_menu(app->view_lan);
            }
            consumed = true;
            break;
        case WlanAppCustomEventLanMenuOk:
            wlan_lan_view_close_menu(app->view_lan);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneSmbDownload);
            consumed = true;
            break;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(wlan_lan_view_is_menu_open(app->view_lan)) {
            wlan_lan_view_close_menu(app->view_lan);
            return true;
        }
        if(s_loading) return true;
        if(app->smb_path[0] != '\0') {
            smb_path_parent(app->smb_path);
            smb_browser_load(app);
            return true;
        }
        if(app->smb_share[0] != '\0') {
            app->smb_share[0] = '\0';
            smb_browser_load(app);
            return true;
        }
        // Top level: end the session and return to the server list.
        wlan_smb_disconnect(app->smb);
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, WlanAppSceneSmbScan);
        return true;
    }
    return consumed;
}

void wlan_app_scene_smb_browser_on_exit(void* context) {
    WlanApp* app = context;
    wlan_lan_view_clear_menu(app->view_lan);
    wlan_lan_view_close_menu(app->view_lan);
    wlan_lan_view_clear(app->view_lan);
    wlan_lan_view_set_empty_text(app->view_lan, NULL);
}
