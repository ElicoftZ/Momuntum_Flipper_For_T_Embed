/* SMB Browser step 4: download the selected file/folder.
 *
 * Runs wlan_smb_op_download into /ext/wifi/smb/<server-ip>/<share>/<path> and
 * shows live progress (files + bytes) in a popup. Back cancels an in-flight
 * download; when finished, Back returns to the browser. */

#include "../wlan_app.h"

#include <storage/storage.h>

#include <string.h>
#include <stdio.h>

// snprintf into fixed buffers; truncation is safe and intentional.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define SMB_DL_ROOT "/ext/wifi/smb"

typedef enum {
    SmbDlPhaseRunning = 0,
    SmbDlPhaseDone,
    SmbDlPhaseError,
} SmbDlPhase;

static SmbDlPhase s_phase;
static char s_text[96];

static void smb_dl_fmt_bytes(uint64_t b, char* out, size_t sz) {
    if(b >= 1024 * 1024)
        snprintf(out, sz, "%lu.%lu MB", (unsigned long)(b / (1024 * 1024)),
                 (unsigned long)((b % (1024 * 1024)) / (1024 * 105)));
    else if(b >= 1024)
        snprintf(out, sz, "%lu KB", (unsigned long)(b / 1024));
    else
        snprintf(out, sz, "%lu B", (unsigned long)b);
}

static void smb_dl_update_popup(WlanApp* app) {
    char bytes[24];
    smb_dl_fmt_bytes(wlan_smb_dl_bytes_done(app->smb), bytes, sizeof(bytes));
    uint32_t files = wlan_smb_dl_files_done(app->smb);

    if(s_phase == SmbDlPhaseRunning) {
        snprintf(s_text, sizeof(s_text), "%lu files\n%s", (unsigned long)files, bytes);
        popup_set_header(app->popup, "Downloading...", 64, 10, AlignCenter, AlignTop);
    } else if(s_phase == SmbDlPhaseDone) {
        snprintf(s_text, sizeof(s_text), "%lu files\n%s", (unsigned long)files, bytes);
        popup_set_header(app->popup, "Done", 64, 10, AlignCenter, AlignTop);
    } else {
        strncpy(s_text, wlan_smb_error(app->smb), sizeof(s_text) - 1);
        s_text[sizeof(s_text) - 1] = '\0';
        popup_set_header(app->popup, "Failed", 64, 10, AlignCenter, AlignTop);
    }
    popup_set_text(app->popup, s_text, 64, 34, AlignCenter, AlignCenter);
}

void wlan_app_scene_smb_download_on_enter(void* context) {
    WlanApp* app = context;
    s_phase = SmbDlPhaseRunning;

    char local_base[80];
    snprintf(local_base, sizeof(local_base), "%s/%s", SMB_DL_ROOT, app->smb_server_ip);

    popup_reset(app->popup);
    strncpy(s_text, "Starting...", sizeof(s_text) - 1);
    s_text[sizeof(s_text) - 1] = '\0';
    popup_set_header(app->popup, "Downloading...", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, s_text, 64, 34, AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);

    wlan_smb_op_download(
        app->smb, app->smb_dl_share, app->smb_dl_path, app->smb_dl_is_dir, local_base);
}

bool wlan_app_scene_smb_download_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        if(s_phase == SmbDlPhaseRunning) {
            WlanSmbState st = wlan_smb_state(app->smb);
            if(st == WlanSmbStateReady) {
                s_phase = SmbDlPhaseDone;
            } else if(st == WlanSmbStateError) {
                s_phase = SmbDlPhaseError;
            }
        }
        smb_dl_update_popup(app);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        if(s_phase == SmbDlPhaseRunning) {
            wlan_smb_cancel(app->smb);
        }
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }
    return consumed;
}

void wlan_app_scene_smb_download_on_exit(void* context) {
    WlanApp* app = context;
    if(wlan_smb_state(app->smb) == WlanSmbStateBusy) {
        wlan_smb_cancel(app->smb);
    }
    popup_reset(app->popup);
}
