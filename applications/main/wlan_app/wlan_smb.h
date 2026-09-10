#pragma once

// SMB client wrapper for the WiFi app's "SMB Browser".
//
// Thin abstraction over a pluggable SMB backend. The only backend today is
// libsmb2 (SMB2/3, with the multi-protocol negotiate that lets it upgrade
// from an SMB1 hello). The public API here is deliberately backend-agnostic
// so a real SMB1-only engine can be added later behind the same calls
// (see wlan_smb.c: WlanSmbBackend).
//
// All blocking libsmb2 work runs on a dedicated worker task (never a
// FuriThread — lwIP sockets must be driven from an xTaskCreate task, see the
// project memory note). Scenes fire an async op and poll wlan_smb_state()
// from their tick handler.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define WLAN_SMB_MAX_ENTRIES 512
#define WLAN_SMB_NAME_MAX 256
#define WLAN_SMB_SHARE_MAX 80
#define WLAN_SMB_USER_MAX 64
#define WLAN_SMB_PASS_MAX 64
#define WLAN_SMB_PATH_MAX 640
#define WLAN_SMB_ERR_MAX 128

typedef enum {
    WlanSmbStateIdle = 0, // nothing running, no result
    WlanSmbStateBusy,     // an op is running on the worker
    WlanSmbStateReady,    // last op succeeded (entries / progress valid)
    WlanSmbStateError,    // last op failed (wlan_smb_error() has the message)
} WlanSmbState;

typedef struct {
    char name[WLAN_SMB_NAME_MAX];
    bool is_dir;
    uint64_t size;
} WlanSmbEntry;

typedef struct WlanSmb WlanSmb;

WlanSmb* wlan_smb_alloc(void);
void wlan_smb_free(WlanSmb* smb);

// --- Async operations (spawn the worker; poll state afterwards) ---

/** Connect to server_ip ("a.b.c.d"), authenticate and enumerate shares.
 *  user/pass empty => anonymous/guest attempt. On success the share list is
 *  exposed as the entry list (every entry is_dir=true). */
void wlan_smb_op_connect(WlanSmb* smb, const char* server_ip, const char* user, const char* pass);

/** List the contents of share:/path. path uses '/' separators, "" = root.
 *  On success the directory contents are exposed as the entry list. */
void wlan_smb_op_list(WlanSmb* smb, const char* share, const char* path);

/** Download share:/remote_path (a single file or, if is_dir, a whole tree)
 *  into local_base, preserving the share/path structure below it. */
void wlan_smb_op_download(
    WlanSmb* smb,
    const char* share,
    const char* remote_path,
    bool is_dir,
    const char* local_base);

// --- Status / results (poll from a scene tick) ---

WlanSmbState wlan_smb_state(WlanSmb* smb);
const char* wlan_smb_error(WlanSmb* smb); // valid when state == Error

/** Entry-list access (valid after a connect/list op reaches Ready; entries
 *  are also readable live while Busy for progressive display). */
uint16_t wlan_smb_entry_count(WlanSmb* smb);
bool wlan_smb_entry(WlanSmb* smb, uint16_t idx, WlanSmbEntry* out);

// --- Download progress (poll while a download op runs) ---

uint32_t wlan_smb_dl_files_done(WlanSmb* smb);
uint32_t wlan_smb_dl_files_total(WlanSmb* smb); // 0 = still counting / single file
uint64_t wlan_smb_dl_bytes_done(WlanSmb* smb);
void wlan_smb_dl_current(WlanSmb* smb, char* out, size_t sz);

/** Request cancellation of the running op and wait for the worker to stop.
 *  Safe to call when idle. */
void wlan_smb_cancel(WlanSmb* smb);

/** Tear down the SMB session/socket. Call when leaving the browser. */
void wlan_smb_disconnect(WlanSmb* smb);
