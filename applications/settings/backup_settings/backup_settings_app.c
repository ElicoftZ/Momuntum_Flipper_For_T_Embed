/**
 * @file backup_settings_app.c
 * @brief Copy the settings that live on internal flash to the SD card, and
 *        restore them again.
 *
 * IMPORTANT: most settings are NOT files. saved_struct maps a path like
 * "/int/.notification.settings" to a key in the NVS namespace "saved_struct"
 * and stores a blob there, so copying files backs up nothing and restoring
 * them changes nothing. Everything reached through saved_struct is therefore
 * backed up by enumerating NVS; the handful of settings that really are files
 * (the Momentum settings text, the main-menu list) are copied as files.
 *
 * NVS survives a firmware reflash but not an erase_flash, and the whole point
 * of a backup here is the factory-reset escape hatch and card swaps.
 *
 * Restore does not reload anything into the running services -- they read their
 * settings once at start -- so it reboots afterwards rather than pretending the
 * change took effect.
 */

#include <furi.h>
#include <furi_hal.h>

#include <gui/gui.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <dolphin/dolphin.h>

#include <esp_system.h>
#include <string.h> // memcmp
#include <nvs.h>
#include <nvs_flash.h>

#define TAG "BackupSettings"

/** Where backups are written on the card. */
#define BACKUP_DIR     EXT_PATH("backup")
/** One file per NVS blob, named after its key. */
#define BACKUP_NVS_DIR EXT_PATH("backup/nvs")

/* The namespace saved_struct writes every settings blob into. */
#define SAVED_STRUCT_NAMESPACE "saved_struct"

#define BACKUP_MSG_LEN 96

/* Real files only. Anything that goes through saved_struct lives in NVS and
 * is handled separately -- listing it here would copy a stale file, or none. */
static const char* const backup_files[] = {
    ".momentum_settings.txt",
    ".mainmenu_apps.txt",
    ".nrf24jam.cfg",
};
#define BACKUP_FILE_COUNT (sizeof(backup_files) / sizeof(backup_files[0]))

typedef enum {
    BackupStateMenu,
    BackupStateBusy,
    BackupStateResult,
} BackupState;

typedef enum {
    BackupMenuBackup = 0,
    BackupMenuRestore,
    BackupMenuCount,
} BackupMenuItem;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;

    BackupState state;
    uint8_t idx;

    /* Set by the key handler, executed by the main loop once the lock is
     * released: 0 = nothing, 1 = backup, 2 = restore. The work cannot run
     * inside the handler because the loop holds app->mutex across it and
     * backup_run() needs that same non-recursive mutex to publish results. */
    uint8_t pending;

    char message[BACKUP_MSG_LEN];
    /* Result screen only: a restore needs a reboot to take effect. */
    bool needs_reboot;
    bool ok;
} Backup;

typedef struct {
    InputEvent input;
} BackupEvent;

/* ------------------------------------------------------------------ */
/* Work                                                                */
/* ------------------------------------------------------------------ */

/** Copy one file, overwriting any existing destination.
 * @return true if copied, false if the source was simply absent. */
static bool backup_copy_one(
    Storage* storage,
    const char* from,
    const char* to,
    FS_Error* first_error) {
    if(!storage_file_exists(storage, from)) return false;

    /* storage_common_copy refuses to overwrite, so clear the target first. */
    if(storage_file_exists(storage, to)) {
        storage_common_remove(storage, to);
    }

    const FS_Error err = storage_common_copy(storage, from, to);
    if(err != FSE_OK) {
        FURI_LOG_E(TAG, "copy %s -> %s: %s", from, to, storage_error_get_desc(err));
        if(*first_error == FSE_OK) *first_error = err;
        return false;
    }
    return true;
}

/** @return true when the file already contains exactly this data. */
static bool backup_file_matches(
    Storage* storage,
    const char* path,
    const uint8_t* blob,
    size_t len) {
    File* file = storage_file_alloc(storage);
    bool same = false;

    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(storage_file_size(file) == len) {
            uint8_t* cur = malloc(len);
            if(cur) {
                same = (storage_file_read(file, cur, (uint16_t)len) == len) &&
                       (memcmp(cur, blob, len) == 0);
                free(cur);
            }
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    return same;
}

/** Write one NVS blob to SD as backup/nvs/<key>.bin. */
static bool backup_nvs_key_to_sd(
    Storage* storage,
    nvs_handle_t nvs,
    const char* key,
    FS_Error* first_error) {
    size_t len = 0;
    if(nvs_get_blob(nvs, key, NULL, &len) != ESP_OK || len == 0) return false;

    uint8_t* blob = malloc(len);
    if(!blob) return false;

    bool ok = false;
    if(nvs_get_blob(nvs, key, blob, &len) == ESP_OK) {
        char path[128];
        snprintf(path, sizeof(path), "%s/%s.bin", BACKUP_NVS_DIR, key);

        /* Skip the rewrite when the card already holds this exact blob. */
        if(backup_file_matches(storage, path, blob, len)) {
            free(blob);
            return true;
        }

        File* file = storage_file_alloc(storage);
        if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            ok = (storage_file_write(file, blob, len) == len);
            if(!ok && *first_error == FSE_OK) *first_error = FSE_INTERNAL;
        } else if(*first_error == FSE_OK) {
            *first_error = storage_file_get_error(file);
        }
        storage_file_close(file);
        storage_file_free(file);
    }

    free(blob);
    return ok;
}

/** Enumerate every blob in the saved_struct namespace and write them out. */
static size_t backup_nvs_all(Storage* storage, FS_Error* first_error) {
    nvs_handle_t nvs;
    if(nvs_open(SAVED_STRUCT_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        FURI_LOG_W(TAG, "no saved_struct namespace yet");
        return 0;
    }

    size_t count = 0;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", SAVED_STRUCT_NAMESPACE, NVS_TYPE_BLOB, &it);
    while(err == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if(backup_nvs_key_to_sd(storage, nvs, info.key, first_error)) count++;
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);

    nvs_close(nvs);
    return count;
}

/** @return true when NVS already holds exactly this blob. */
static bool nvs_blob_matches(nvs_handle_t nvs, const char* key, const uint8_t* blob, size_t len) {
    size_t cur_len = 0;
    if(nvs_get_blob(nvs, key, NULL, &cur_len) != ESP_OK) return false;
    if(cur_len != len) return false;

    uint8_t* cur = malloc(cur_len);
    if(!cur) return false;

    bool same = (nvs_get_blob(nvs, key, cur, &cur_len) == ESP_OK) &&
                (memcmp(cur, blob, cur_len) == 0);
    free(cur);
    return same;
}

/** Read the blobs under backup/nvs back into the saved_struct namespace. */
static size_t restore_nvs_all(Storage* storage, FS_Error* first_error) {
    File* dir = storage_file_alloc(storage);
    if(!storage_dir_open(dir, BACKUP_NVS_DIR)) {
        storage_file_free(dir);
        return 0;
    }

    nvs_handle_t nvs;
    if(nvs_open(SAVED_STRUCT_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        storage_dir_close(dir);
        storage_file_free(dir);
        if(*first_error == FSE_OK) *first_error = FSE_INTERNAL;
        return 0;
    }

    size_t count = 0;
    size_t written = 0;
    size_t unchanged = 0;
    char name[64];
    FileInfo info;
    while(storage_dir_read(dir, &info, name, sizeof(name))) {
        if(info.flags & FSF_DIRECTORY) continue;

        /* Strip the .bin suffix to recover the NVS key. */
        char key[16];
        const char* dot = strrchr(name, '.');
        size_t key_len = dot ? (size_t)(dot - name) : strlen(name);
        if(key_len == 0 || key_len >= sizeof(key)) continue;
        memcpy(key, name, key_len);
        key[key_len] = '\0';

        char path[128];
        snprintf(path, sizeof(path), "%s/%s", BACKUP_NVS_DIR, name);

        File* file = storage_file_alloc(storage);
        if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            const uint64_t size = storage_file_size(file);
            if(size > 0 && size < 4096) {
                uint8_t* blob = malloc((size_t)size);
                if(blob) {
                    if(storage_file_read(file, blob, (uint16_t)size) == size) {
                        /* NVS is flash. Rewriting a blob that already matches
                         * spends an erase/write cycle to change nothing, and a
                         * restore is usually mostly-identical data. */
                        if(nvs_blob_matches(nvs, key, blob, (size_t)size)) {
                            count++;
                            unchanged++;
                        } else if(nvs_set_blob(nvs, key, blob, (size_t)size) == ESP_OK) {
                            count++;
                            written++;
                        }
                    }
                    free(blob);
                }
            }
        } else if(*first_error == FSE_OK) {
            *first_error = storage_file_get_error(file);
        }
        storage_file_close(file);
        storage_file_free(file);
    }

    /* Only commit when something actually changed. */
    if(written > 0) nvs_commit(nvs);
    nvs_close(nvs);
    FURI_LOG_I(TAG, "restore: %u written, %u already current", (unsigned)written, (unsigned)unchanged);

    storage_dir_close(dir);
    storage_file_free(dir);
    return count;
}

/** @param to_sd true = internal -> SD (backup), false = SD -> internal. */
static void backup_run(Backup* app, bool to_sd) {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    size_t copied = 0;
    FS_Error first_error = FSE_OK;

    /* Create it either way. On restore this leaves an obvious, correctly
     * named folder for the user to drop a backup into, instead of failing
     * against a path that does not exist yet. */
    storage_simply_mkdir(storage, BACKUP_DIR);
    storage_simply_mkdir(storage, BACKUP_NVS_DIR);

    /* Force the dolphin to persist its XP/level/mood to NVS before we read it.
     * Earned XP is not written immediately -- dolphin schedules a flush on a
     * delay timer -- so a backup taken right after leveling up would otherwise
     * capture stale progress. The state itself already rides out in the NVS
     * sweep below (key "dolphin_state" in the saved_struct namespace); this only
     * makes sure the sweep sees the latest value. Backup direction only. */
    if(to_sd) {
        Dolphin* dolphin = furi_record_open(RECORD_DOLPHIN);
        dolphin_flush(dolphin);
        furi_record_close(RECORD_DOLPHIN);
    }

    for(size_t i = 0; i < BACKUP_FILE_COUNT; i++) {
        char internal[128];
        char sd[128];
        /* INT_PATH(x) is "/int" "/" x, so INT_PATH("") already ends in a
         * slash -- joining with another one gave "/int//.name", which could
         * still be opened for reading but failed when creating the file. */
        snprintf(
            internal, sizeof(internal), "%s/%s", STORAGE_INT_PATH_PREFIX, backup_files[i]);
        snprintf(sd, sizeof(sd), "%s/%s", BACKUP_DIR, backup_files[i]);

        const char* from = to_sd ? internal : sd;
        const char* to = to_sd ? sd : internal;

        if(backup_copy_one(storage, from, to, &first_error)) copied++;
    }

    /* The settings that actually matter live here, not in those files. */
    if(to_sd) {
        copied += backup_nvs_all(storage, &first_error);
    } else {
        copied += restore_nvs_all(storage, &first_error);
    }

    furi_record_close(RECORD_STORAGE);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    const bool failed = (first_error != FSE_OK);
    app->ok = !failed && copied > 0;

    if(copied == 0 && !failed) {
        strlcpy(
            app->message,
            to_sd ? "Nothing to back up" : "Empty: put files in SD:/backup",
            BACKUP_MSG_LEN);
    } else if(failed) {
        /* Name the error: "failed" alone gives nothing to act on. */
        snprintf(
            app->message,
            BACKUP_MSG_LEN,
            "Copied %u, %s",
            (unsigned)copied,
            storage_error_get_desc(first_error));
    } else {
        snprintf(
            app->message,
            BACKUP_MSG_LEN,
            to_sd ? "Saved %u files to SD" : "Restored %u files",
            (unsigned)copied);
    }

    /* Services read their settings once at startup, so a restore is not live. */
    app->needs_reboot = !to_sd && app->ok;
    app->state = BackupStateResult;
    furi_mutex_release(app->mutex);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void backup_render_menu(Canvas* canvas, Backup* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignCenter, "Backup Settings");

    canvas_set_font(canvas, FontSecondary);
    static const char* const items[BackupMenuCount] = {"Save to SD", "Restore from SD"};

    for(uint8_t i = 0; i < BackupMenuCount; i++) {
        const uint8_t y = 28 + (uint8_t)(i * 14);
        canvas_draw_str_aligned(canvas, 64, y, AlignCenter, AlignCenter, items[i]);
        if(app->idx == i) {
            elements_frame(canvas, 10, y - 6, 108, 13);
        }
    }

    canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignCenter, "SD:/backup");
}

static void backup_render_busy(Canvas* canvas, Backup* app) {
    UNUSED(app);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "Working...");
}

static void backup_render_result(Canvas* canvas, Backup* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas, 64, 12, AlignCenter, AlignCenter, app->ok ? "Done" : "Failed");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, app->message);

    if(app->needs_reboot) {
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, "OK: reboot to apply");
        canvas_draw_str_aligned(canvas, 64, 56, AlignCenter, AlignCenter, "Back: later");
    } else {
        canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignCenter, "Press any key");
    }
}

static void backup_render_callback(Canvas* canvas, void* ctx) {
    Backup* app = ctx;
    if(furi_mutex_acquire(app->mutex, 200) != FuriStatusOk) return;

    switch(app->state) {
    case BackupStateMenu:
        backup_render_menu(canvas, app);
        break;
    case BackupStateBusy:
        backup_render_busy(canvas, app);
        break;
    case BackupStateResult:
        backup_render_result(canvas, app);
        break;
    }

    furi_mutex_release(app->mutex);
}

static void backup_input_callback(InputEvent* input_event, void* ctx) {
    Backup* app = ctx;
    BackupEvent event = {.input = *input_event};
    furi_message_queue_put(app->queue, &event, FuriWaitForever);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/** @return false when the app should exit. */
static bool backup_handle_key(Backup* app, const InputEvent* input) {
    if(input->type != InputTypeShort && input->type != InputTypeRepeat) return true;

    switch(app->state) {
    case BackupStateMenu:
        if(input->key == InputKeyBack) return false;

        if(input->key == InputKeyUp) {
            app->idx = (app->idx == 0) ? BackupMenuCount - 1 : app->idx - 1;
        } else if(input->key == InputKeyDown) {
            app->idx = (app->idx + 1 >= BackupMenuCount) ? 0 : app->idx + 1;
        } else if(input->key == InputKeyOk) {
            app->state = BackupStateBusy;
            app->pending = (app->idx == BackupMenuBackup) ? 1 : 2;
        }
        break;

    case BackupStateBusy:
        break;

    case BackupStateResult:
        if(app->needs_reboot && input->key == InputKeyOk) {
            FURI_LOG_I(TAG, "Rebooting to apply restored settings");
            esp_restart();
        }
        app->state = BackupStateMenu;
        break;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int32_t backup_settings_app(void* p) {
    UNUSED(p);

    Backup* app = malloc(sizeof(Backup));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(BackupEvent));
    app->state = BackupStateMenu;
    app->idx = 0;
    app->pending = 0;
    app->needs_reboot = false;
    app->ok = false;
    app->message[0] = '\0';

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, backup_render_callback, app);
    view_port_input_callback_set(app->view_port, backup_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    BackupEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        running = backup_handle_key(app, &event.input);
        const uint8_t pending = app->pending;
        app->pending = 0;
        furi_mutex_release(app->mutex);

        /* Draw "Working..." before starting: the copy blocks this thread,
         * and the render callback needs the lock we just released. */
        view_port_update(app->view_port);

        if(running && pending != 0) {
            /* A handful of small files, so inline is fine; a worker thread
             * would only add a way to leave mid-write. */
            backup_run(app, pending == 1);
            view_port_update(app->view_port);
        }
    }

    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
