#include "wlan_smb.h"

#include <furi.h>
#include <storage/storage.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

// libsmb2 headers assume the autoconf-included POSIX headers are already in
// scope (time_t, off_t, sockaddr, ...); pull them in first.
#include <time.h>
#include <sys/types.h>
#include <fcntl.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-share-enum.h>

#include <string.h>
#include <stdlib.h>

// Remote/local path joins use snprintf into fixed buffers; truncation is safe
// (always NUL-terminated) and intentional for pathological path lengths.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define TAG "WlanSmb"

// The worker stack MUST be internal DRAM: it runs concurrently with WiFi,
// which briefly disables the flash cache for internal ops. A PSRAM stack is
// unreachable during those windows -> "Cache disabled but cached memory
// region accessed" panic (crashed in libsmb2 NTOWFv2 during session setup).
// Same reasoning as wlan_hal's worker (also internal DRAM).
// libsmb2's session-setup path (NTLMSSP + SPNEGO/ASN.1 + recv processing) is
// deep; 16K overflowed and corrupted neighbouring heap (wild-pointer crash).
#define WLAN_SMB_WORKER_STACK 32768
#define WLAN_SMB_DL_CHUNK (32 * 1024)
#define WLAN_SMB_TIMEOUT_S 8

typedef enum {
    SmbCmdNone = 0,
    SmbCmdConnect,
    SmbCmdList,
    SmbCmdDownload,
    SmbCmdQuit,
} SmbCmdType;

// ---------------------------------------------------------------------------
// Backend abstraction. Only libsmb2 (SMB2/3) exists today; a real SMB1-only
// engine can be added by providing a second WlanSmbBackend and selecting it
// per server (e.g. after a failed SMB2 negotiate). Every entry point runs on
// the worker task and returns 0 on success / -1 on failure (message in
// smb->error).
// ---------------------------------------------------------------------------
struct WlanSmb;
typedef struct {
    const char* name;
    int (*connect)(struct WlanSmb* s, const char* server, const char* user, const char* pass);
    int (*enum_shares)(struct WlanSmb* s);
    int (*list)(struct WlanSmb* s, const char* share, const char* path);
    int (*download)(
        struct WlanSmb* s,
        const char* share,
        const char* rpath,
        bool is_dir,
        const char* local_base);
    void (*disconnect)(struct WlanSmb* s);
} WlanSmbBackend;

struct WlanSmb {
    const WlanSmbBackend* backend;

    // Worker task + command channel. The TCB (StaticTask_t) must live in
    // internal DRAM even though the stack may be in PSRAM — FreeRTOS asserts
    // xPortCheckValidTCBMem() on it. Hence task_buf is a separately-allocated
    // internal-DRAM pointer, not an inline field (this whole struct is PSRAM).
    TaskHandle_t task;
    StaticTask_t* task_buf;
    StackType_t* stack;
    QueueHandle_t cmd_queue;

    volatile WlanSmbState state;
    volatile bool cancel;
    char error[WLAN_SMB_ERR_MAX];

    // Connection identity (kept across ops so list/download can reconnect the
    // share with the same credentials).
    char server[64];
    char user[WLAN_SMB_USER_MAX];
    char pass[WLAN_SMB_PASS_MAX];

    // Pending op arguments (written by the caller before enqueuing).
    SmbCmdType pending_type;
    char arg_share[WLAN_SMB_SHARE_MAX];
    char arg_path[WLAN_SMB_PATH_MAX];
    char arg_local[WLAN_SMB_PATH_MAX];
    bool arg_is_dir;

    // Result: entry list (PSRAM).
    WlanSmbEntry* entries;
    volatile uint16_t entry_count;

    // Download progress.
    volatile uint32_t dl_files_done;
    volatile uint32_t dl_files_total;
    volatile uint64_t dl_bytes_done;
    char dl_current[WLAN_SMB_NAME_MAX];

    // libsmb2 backend state.
    struct smb2_context* smb2;
    uint8_t* chunk; // PSRAM I/O buffer

    Storage* storage;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static void smb_set_error(WlanSmb* s, const char* msg) {
    strncpy(s->error, msg ? msg : "unknown error", sizeof(s->error) - 1);
    s->error[sizeof(s->error) - 1] = '\0';
}

static void smb_entries_reset(WlanSmb* s) {
    s->entry_count = 0;
}

// Sort: directories first, then files, each case-insensitive alphabetical.
static int smb_entry_cmp(const void* a, const void* b) {
    const WlanSmbEntry* ea = a;
    const WlanSmbEntry* eb = b;
    if(ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    return strcasecmp(ea->name, eb->name);
}

static void smb_entries_sort(WlanSmb* s) {
    if(s->entry_count > 1) qsort(s->entries, s->entry_count, sizeof(WlanSmbEntry), smb_entry_cmp);
}

static WlanSmbEntry* smb_entry_push(WlanSmb* s) {
    if(s->entry_count >= WLAN_SMB_MAX_ENTRIES) return NULL;
    WlanSmbEntry* e = &s->entries[s->entry_count++];
    memset(e, 0, sizeof(*e));
    return e;
}

// Create every missing directory level of an /ext/... path.
static void smb_mkdir_p(Storage* st, const char* path) {
    char buf[WLAN_SMB_PATH_MAX];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    // Skip leading "/ext" mount prefix; create each subsequent segment.
    for(char* p = buf + 1; *p; ++p) {
        if(*p == '/') {
            *p = '\0';
            storage_simply_mkdir(st, buf);
            *p = '/';
        }
    }
    storage_simply_mkdir(st, buf);
}

// Replace characters FatFS can't store; SMB names may contain ':' '*' '?' etc.
static void smb_sanitize_component(char* name) {
    for(char* p = name; *p; ++p) {
        char c = *p;
        if(c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
           c == '>' || c == '|') {
            *p = '_';
        }
    }
}

// ===========================================================================
// libsmb2 backend
// ===========================================================================
static int smb2_be_apply_error(WlanSmb* s, const char* what) {
    const char* e = s->smb2 ? smb2_get_error(s->smb2) : NULL;
    char msg[WLAN_SMB_ERR_MAX];
    snprintf(msg, sizeof(msg), "%s: %s", what, (e && *e) ? e : "failed");
    smb_set_error(s, msg);
    ESP_LOGW(TAG, "%s", msg);
    return -1;
}

static void smb2_be_disconnect(WlanSmb* s) {
    if(s->smb2) {
        smb2_disconnect_share(s->smb2);
        smb2_destroy_context(s->smb2);
        s->smb2 = NULL;
    }
}

static struct smb2_context* smb2_be_new_ctx(WlanSmb* s) {
    struct smb2_context* c = smb2_init_context();
    if(!c) return NULL;
    smb2_set_security_mode(c, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_timeout(c, WLAN_SMB_TIMEOUT_S);
    // Always set an explicit user, even "" — otherwise libsmb2 keeps the
    // init_context default (getlogin_r path) whose user pointer got corrupted
    // on the guest path, crashing in NTOWFv2 (strlen(user)).
    smb2_set_user(c, s->user);
    // Only set a password when we actually have one. With no password the
    // context keeps password==NULL, so libsmb2 does an anonymous session-setup
    // and skips the NTLMv2 hash path entirely (the guest crash path).
    if(s->pass[0]) smb2_set_password(c, s->pass);
    return c;
}

// Open a fresh context and tree-connect to `share`. Leaves s->smb2 live.
static int smb2_be_open_share(WlanSmb* s, const char* share) {
    smb2_be_disconnect(s);
    s->smb2 = smb2_be_new_ctx(s);
    if(!s->smb2) {
        smb_set_error(s, "out of memory");
        return -1;
    }
    if(smb2_connect_share(s->smb2, s->server, share, s->user[0] ? s->user : "") < 0) {
        int rc = smb2_be_apply_error(s, "connect");
        smb2_be_disconnect(s);
        return rc;
    }
    return 0;
}

static int smb2_be_connect(WlanSmb* s, const char* server, const char* user, const char* pass) {
    strncpy(s->server, server, sizeof(s->server) - 1);
    s->server[sizeof(s->server) - 1] = '\0';
    strncpy(s->user, user ? user : "", sizeof(s->user) - 1);
    s->user[sizeof(s->user) - 1] = '\0';
    strncpy(s->pass, pass ? pass : "", sizeof(s->pass) - 1);
    s->pass[sizeof(s->pass) - 1] = '\0';

    // Validate credentials by connecting to IPC$, then drop the connection —
    // the context keeps the credentials for later per-share reconnects.
    if(smb2_be_open_share(s, "IPC$") < 0) return -1;
    smb2_be_disconnect(s);
    return 0;
}

static int smb2_be_enum_shares(WlanSmb* s) {
    smb_entries_reset(s);
    if(smb2_be_open_share(s, "IPC$") < 0) return -1;
    struct srvsvc_NetrShareEnum_rep* rep = smb2_share_enum_sync(s->smb2, SHARE_INFO_1);
    if(!rep) {
        // Login worked but enum was refused — expose an empty share list
        // rather than failing the whole login.
        ESP_LOGW(TAG, "share enum failed: %s", smb2_get_error(s->smb2));
        smb2_be_disconnect(s);
        return 0;
    }

    struct srvsvc_SHARE_INFO_1_CONTAINER* c1 = &rep->ses.ShareEnum.Level1;
    for(uint32_t i = 0; i < c1->EntriesRead; ++i) {
        struct srvsvc_SHARE_INFO_1* si = &c1->share_info_1[i];
        if(!si->netname) continue;
        uint32_t stype = si->type & 0x0F;
        if(stype != SRVSVC_SHARE_TYPE_DISKTREE) continue; // skip IPC$/printers
        WlanSmbEntry* e = smb_entry_push(s);
        if(!e) break;
        strncpy(e->name, si->netname, sizeof(e->name) - 1);
        e->is_dir = true;
        e->size = 0;
    }
    smb2_free_data(s->smb2, rep);
    smb_entries_sort(s);
    smb2_be_disconnect(s); // reconnect per share when the user picks one
    return 0;
}

static int smb2_be_list(WlanSmb* s, const char* share, const char* path) {
    smb_entries_reset(s);
    if(smb2_be_open_share(s, share) < 0) return -1;

    struct smb2dir* dir = smb2_opendir(s->smb2, path ? path : "");
    if(!dir) {
        int rc = smb2_be_apply_error(s, "opendir");
        smb2_be_disconnect(s);
        return rc;
    }

    struct smb2dirent* ent;
    while((ent = smb2_readdir(s->smb2, dir)) != NULL) {
        if(!ent->name) continue;
        if(strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) continue;
        WlanSmbEntry* e = smb_entry_push(s);
        if(!e) break;
        strncpy(e->name, ent->name, sizeof(e->name) - 1);
        e->is_dir = (ent->st.smb2_type == SMB2_TYPE_DIRECTORY);
        e->size = ent->st.smb2_size;
    }
    smb2_closedir(s->smb2, dir);
    smb_entries_sort(s);
    smb2_be_disconnect(s);
    return 0;
}

// Download one already-open file handle to a local path. Returns 0/-1.
static int smb2_be_pull_file(
    WlanSmb* s,
    const char* remote,
    uint64_t size,
    const char* local_path) {
    struct smb2fh* fh = smb2_open(s->smb2, remote, O_RDONLY);
    if(!fh) return smb2_be_apply_error(s, "open");

    File* out = storage_file_alloc(s->storage);
    if(!storage_file_open(out, local_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(out);
        smb2_close(s->smb2, fh);
        smb_set_error(s, "cannot write to SD");
        return -1;
    }

    strncpy(s->dl_current, remote, sizeof(s->dl_current) - 1);
    s->dl_current[sizeof(s->dl_current) - 1] = '\0';

    int rc = 0;
    for(;;) {
        if(s->cancel) {
            rc = -1;
            smb_set_error(s, "cancelled");
            break;
        }
        int n = smb2_read(s->smb2, fh, s->chunk, WLAN_SMB_DL_CHUNK);
        if(n < 0) {
            rc = smb2_be_apply_error(s, "read");
            break;
        }
        if(n == 0) break; // EOF (authoritative; do not rely on stat size)
        if(storage_file_write(out, s->chunk, n) != (size_t)n) {
            smb_set_error(s, "SD write error");
            rc = -1;
            break;
        }
        s->dl_bytes_done += (uint64_t)n;
    }
    (void)size;

    storage_file_close(out);
    storage_file_free(out);
    smb2_close(s->smb2, fh);
    return rc;
}

// Recursively pull remote dir `rpath` (relative to the share root) into the
// local directory `local_dir`. Names are copied per level before descending
// so no libsmb2 dir handle stays open across recursion.
static int smb2_be_pull_dir(WlanSmb* s, const char* rpath, const char* local_dir) {
    if(s->cancel) return -1;

    struct smb2dir* dir = smb2_opendir(s->smb2, rpath[0] ? rpath : "");
    if(!dir) return smb2_be_apply_error(s, "opendir");

    // Snapshot this level (readdir pointers are invalidated by closedir).
    typedef struct {
        char name[WLAN_SMB_NAME_MAX];
        bool is_dir;
        uint64_t size;
    } Snap;
    uint32_t cap = 256, cnt = 0;
    Snap* snap = heap_caps_malloc(cap * sizeof(Snap), MALLOC_CAP_SPIRAM);
    if(!snap) {
        smb2_closedir(s->smb2, dir);
        smb_set_error(s, "out of memory");
        return -1;
    }

    struct smb2dirent* ent;
    while((ent = smb2_readdir(s->smb2, dir)) != NULL) {
        if(!ent->name) continue;
        if(strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) continue;
        if(cnt == cap) {
            uint32_t ncap = cap * 2;
            Snap* n = heap_caps_realloc(snap, ncap * sizeof(Snap), MALLOC_CAP_SPIRAM);
            if(!n) break; // keep what we have
            snap = n;
            cap = ncap;
        }
        strncpy(snap[cnt].name, ent->name, sizeof(snap[cnt].name) - 1);
        snap[cnt].name[sizeof(snap[cnt].name) - 1] = '\0';
        snap[cnt].is_dir = (ent->st.smb2_type == SMB2_TYPE_DIRECTORY);
        snap[cnt].size = ent->st.smb2_size;
        cnt++;
    }
    smb2_closedir(s->smb2, dir);

    smb_mkdir_p(s->storage, local_dir);

    int rc = 0;
    for(uint32_t i = 0; i < cnt && rc == 0; ++i) {
        if(s->cancel) {
            rc = -1;
            break;
        }
        char child_remote[WLAN_SMB_PATH_MAX];
        snprintf(
            child_remote, sizeof(child_remote), "%s%s%s", rpath, rpath[0] ? "/" : "", snap[i].name);

        char local_name[WLAN_SMB_NAME_MAX];
        strncpy(local_name, snap[i].name, sizeof(local_name) - 1);
        local_name[sizeof(local_name) - 1] = '\0';
        smb_sanitize_component(local_name);

        char child_local[WLAN_SMB_PATH_MAX];
        snprintf(child_local, sizeof(child_local), "%s/%s", local_dir, local_name);

        if(snap[i].is_dir) {
            rc = smb2_be_pull_dir(s, child_remote, child_local);
        } else {
            rc = smb2_be_pull_file(s, child_remote, snap[i].size, child_local);
            if(rc == 0) s->dl_files_done++;
        }
    }
    free(snap);
    return rc;
}

static int
    smb2_be_download(WlanSmb* s, const char* share, const char* rpath, bool is_dir, const char* local_base) {
    s->dl_files_done = 0;
    s->dl_files_total = is_dir ? 0 : 1;
    s->dl_bytes_done = 0;
    s->dl_current[0] = '\0';

    if(smb2_be_open_share(s, share) < 0) return -1;

    // Local layout: <local_base>/<share>/<rpath>. Sanitize each remote path
    // component so FatFS accepts it.
    char share_clean[WLAN_SMB_SHARE_MAX];
    strncpy(share_clean, share, sizeof(share_clean) - 1);
    share_clean[sizeof(share_clean) - 1] = '\0';
    smb_sanitize_component(share_clean);

    // Build the local directory that will contain the target.
    char rel_dir[WLAN_SMB_PATH_MAX]; // remote parent path, sanitized, '/'-joined
    rel_dir[0] = '\0';
    const char* slash = strrchr(rpath, '/');
    if(slash) {
        size_t plen = (size_t)(slash - rpath);
        if(plen >= sizeof(rel_dir)) plen = sizeof(rel_dir) - 1;
        memcpy(rel_dir, rpath, plen);
        rel_dir[plen] = '\0';
    }

    // Sanitize each segment of rel_dir.
    char rel_dir_clean[WLAN_SMB_PATH_MAX];
    rel_dir_clean[0] = '\0';
    {
        char tmp[WLAN_SMB_PATH_MAX];
        strncpy(tmp, rel_dir, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char* save = NULL;
        char* seg = strtok_r(tmp, "/", &save);
        while(seg) {
            smb_sanitize_component(seg);
            size_t cur = strlen(rel_dir_clean);
            snprintf(
                rel_dir_clean + cur, sizeof(rel_dir_clean) - cur, "%s%s", cur ? "/" : "", seg);
            seg = strtok_r(NULL, "/", &save);
        }
    }

    const char* base_name = slash ? slash + 1 : rpath;
    char base_clean[WLAN_SMB_NAME_MAX];
    strncpy(base_clean, base_name, sizeof(base_clean) - 1);
    base_clean[sizeof(base_clean) - 1] = '\0';
    smb_sanitize_component(base_clean);

    // Parent dir on SD: <local_base>/<share>[/<rel_dir_clean>]
    char parent_local[WLAN_SMB_PATH_MAX];
    if(rel_dir_clean[0]) {
        snprintf(
            parent_local, sizeof(parent_local), "%s/%s/%s", local_base, share_clean, rel_dir_clean);
    } else {
        snprintf(parent_local, sizeof(parent_local), "%s/%s", local_base, share_clean);
    }
    smb_mkdir_p(s->storage, parent_local);

    // target_local is the download destination. For a whole-share download
    // (rpath == "") base_clean is empty; the share dir itself is the target.
    char target_local[WLAN_SMB_PATH_MAX];
    if(base_clean[0]) {
        snprintf(target_local, sizeof(target_local), "%s/%s", parent_local, base_clean);
    } else {
        strncpy(target_local, parent_local, sizeof(target_local) - 1);
        target_local[sizeof(target_local) - 1] = '\0';
    }

    int rc;
    if(is_dir) {
        rc = smb2_be_pull_dir(s, rpath, target_local);
    } else {
        rc = smb2_be_pull_file(s, rpath, 0, target_local);
        if(rc == 0) s->dl_files_done = 1;
    }
    smb2_be_disconnect(s);
    return rc;
}

static const WlanSmbBackend g_smb2_backend = {
    .name = "libsmb2 (SMB2/3)",
    .connect = smb2_be_connect,
    .enum_shares = smb2_be_enum_shares,
    .list = smb2_be_list,
    .download = smb2_be_download,
    .disconnect = smb2_be_disconnect,
};

// ===========================================================================
// Worker task
// ===========================================================================
static void smb_worker(void* ctx) {
    WlanSmb* s = ctx;
    SmbCmdType cmd;
    for(;;) {
        if(xQueueReceive(s->cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if(cmd == SmbCmdQuit) break;

        s->cancel = false;
        int rc = -1;
        switch(cmd) {
        case SmbCmdConnect:
            rc = s->backend->connect(s, s->server, s->user, s->pass);
            break;
        case SmbCmdList:
            // Empty share = top-level share listing (enumerate), otherwise a
            // real directory listing within the share.
            if(s->arg_share[0] == '\0')
                rc = s->backend->enum_shares(s);
            else
                rc = s->backend->list(s, s->arg_share, s->arg_path);
            break;
        case SmbCmdDownload:
            rc = s->backend->download(
                s, s->arg_share, s->arg_path, s->arg_is_dir, s->arg_local);
            break;
        default:
            break;
        }
        s->state = (rc == 0) ? WlanSmbStateReady : WlanSmbStateError;
    }
    vTaskDelete(NULL);
}

// ===========================================================================
// Public API
// ===========================================================================
WlanSmb* wlan_smb_alloc(void) {
    WlanSmb* s = heap_caps_calloc(1, sizeof(WlanSmb), MALLOC_CAP_SPIRAM);
    if(!s) return NULL;
    s->backend = &g_smb2_backend;
    s->state = WlanSmbStateIdle;
    s->storage = furi_record_open(RECORD_STORAGE);

    s->entries = heap_caps_malloc(sizeof(WlanSmbEntry) * WLAN_SMB_MAX_ENTRIES, MALLOC_CAP_SPIRAM);
    s->chunk = heap_caps_malloc(WLAN_SMB_DL_CHUNK, MALLOC_CAP_SPIRAM);
    s->stack = heap_caps_malloc(
        WLAN_SMB_WORKER_STACK * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // TCB must be internal DRAM (FreeRTOS asserts on it).
    s->task_buf = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s->cmd_queue = xQueueCreate(2, sizeof(SmbCmdType));

    if(!s->entries || !s->chunk || !s->stack || !s->task_buf || !s->cmd_queue) {
        ESP_LOGE(TAG, "alloc failed");
        wlan_smb_free(s);
        return NULL;
    }

    s->task = xTaskCreateStaticPinnedToCore(
        smb_worker, "SmbWorker", WLAN_SMB_WORKER_STACK, s, 4, s->stack, s->task_buf, 0);
    if(!s->task) {
        ESP_LOGE(TAG, "task create failed");
        wlan_smb_free(s);
        return NULL;
    }
    return s;
}

void wlan_smb_free(WlanSmb* s) {
    if(!s) return;
    if(s->task) {
        wlan_smb_cancel(s);
        SmbCmdType quit = SmbCmdQuit;
        xQueueSend(s->cmd_queue, &quit, portMAX_DELAY);
        // Wait for the worker to run vTaskDelete on itself.
        for(int i = 0; i < 200 && eTaskGetState(s->task) != eDeleted; ++i) {
            furi_delay_ms(5);
        }
    }
    if(s->smb2) {
        smb2_disconnect_share(s->smb2);
        smb2_destroy_context(s->smb2);
    }
    if(s->cmd_queue) vQueueDelete(s->cmd_queue);
    if(s->storage) furi_record_close(RECORD_STORAGE);
    if(s->entries) free(s->entries);
    if(s->chunk) free(s->chunk);
    if(s->stack) free(s->stack);
    if(s->task_buf) free(s->task_buf);
    free(s);
}

static void smb_enqueue(WlanSmb* s, SmbCmdType cmd) {
    s->state = WlanSmbStateBusy;
    xQueueSend(s->cmd_queue, &cmd, portMAX_DELAY);
}

void wlan_smb_op_connect(WlanSmb* s, const char* server_ip, const char* user, const char* pass) {
    if(!s) return;
    strncpy(s->server, server_ip, sizeof(s->server) - 1);
    s->server[sizeof(s->server) - 1] = '\0';
    strncpy(s->user, user ? user : "", sizeof(s->user) - 1);
    s->user[sizeof(s->user) - 1] = '\0';
    strncpy(s->pass, pass ? pass : "", sizeof(s->pass) - 1);
    s->pass[sizeof(s->pass) - 1] = '\0';
    smb_enqueue(s, SmbCmdConnect);
}

void wlan_smb_op_list(WlanSmb* s, const char* share, const char* path) {
    if(!s) return;
    strncpy(s->arg_share, share, sizeof(s->arg_share) - 1);
    s->arg_share[sizeof(s->arg_share) - 1] = '\0';
    strncpy(s->arg_path, path ? path : "", sizeof(s->arg_path) - 1);
    s->arg_path[sizeof(s->arg_path) - 1] = '\0';
    smb_enqueue(s, SmbCmdList);
}

void wlan_smb_op_download(
    WlanSmb* s,
    const char* share,
    const char* remote_path,
    bool is_dir,
    const char* local_base) {
    if(!s) return;
    strncpy(s->arg_share, share, sizeof(s->arg_share) - 1);
    s->arg_share[sizeof(s->arg_share) - 1] = '\0';
    strncpy(s->arg_path, remote_path ? remote_path : "", sizeof(s->arg_path) - 1);
    s->arg_path[sizeof(s->arg_path) - 1] = '\0';
    strncpy(s->arg_local, local_base, sizeof(s->arg_local) - 1);
    s->arg_local[sizeof(s->arg_local) - 1] = '\0';
    s->arg_is_dir = is_dir;
    smb_enqueue(s, SmbCmdDownload);
}

WlanSmbState wlan_smb_state(WlanSmb* s) {
    return s ? s->state : WlanSmbStateIdle;
}

const char* wlan_smb_error(WlanSmb* s) {
    return s ? s->error : "";
}

uint16_t wlan_smb_entry_count(WlanSmb* s) {
    return s ? s->entry_count : 0;
}

bool wlan_smb_entry(WlanSmb* s, uint16_t idx, WlanSmbEntry* out) {
    if(!s || idx >= s->entry_count) return false;
    *out = s->entries[idx];
    return true;
}

uint32_t wlan_smb_dl_files_done(WlanSmb* s) {
    return s ? s->dl_files_done : 0;
}
uint32_t wlan_smb_dl_files_total(WlanSmb* s) {
    return s ? s->dl_files_total : 0;
}
uint64_t wlan_smb_dl_bytes_done(WlanSmb* s) {
    return s ? s->dl_bytes_done : 0;
}
void wlan_smb_dl_current(WlanSmb* s, char* out, size_t sz) {
    if(!s || sz == 0) return;
    strncpy(out, s->dl_current, sz - 1);
    out[sz - 1] = '\0';
}

void wlan_smb_cancel(WlanSmb* s) {
    if(!s) return;
    s->cancel = true;
    for(int i = 0; i < 600 && s->state == WlanSmbStateBusy; ++i) {
        furi_delay_ms(10);
    }
}

void wlan_smb_disconnect(WlanSmb* s) {
    if(!s) return;
    wlan_smb_cancel(s);
    if(s->smb2) {
        smb2_disconnect_share(s->smb2);
        smb2_destroy_context(s->smb2);
        s->smb2 = NULL;
    }
    s->state = WlanSmbStateIdle;
}
