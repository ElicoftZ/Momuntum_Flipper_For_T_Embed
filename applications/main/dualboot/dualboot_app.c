/**
 * @file dualboot_app.c
 * @brief Install a second firmware into the ota_0 slot and optionally boot it.
 *
 * The board carries two app partitions: this port lives in `factory`, and
 * `ota_0` holds whatever second firmware the user picked. Only one second
 * firmware is resident at a time -- installing a new one overwrites the old,
 * which is why the bins live on the SD card rather than in flash.
 *
 * Coming back here is the fallback direction for free: esp_ota_set_boot_partition()
 * on a *factory* partition erases otadata rather than writing a slot number, and
 * an erased or corrupt otadata makes the ROM bootloader start factory. A failed
 * or half-written install therefore lands back on this firmware rather than
 * bricking the board.
 *
 * Deliberately built on a plain ViewPort and a message queue (the clock app's
 * pattern) rather than a ViewDispatcher: the board has only Up/Down/OK/Back, so
 * handling input directly keeps every element reachable by rotation alone.
 */

#include <furi.h>
#include <furi_hal.h>

#include <gui/gui.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <saved_struct.h>

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_app_format.h>

#include <strings.h> // strcasecmp

#define TAG "DualBoot"

/* Remembers only whether the escape-hatch screen has been dismissed for
 * good. Kept out of any shared settings struct so it cannot be lost to an
 * unrelated version bump. */
#define DUALBOOT_CFG_PATH    INT_PATH(".dualboot.settings")
#define DUALBOOT_CFG_MAGIC   0x4442
#define DUALBOOT_CFG_VERSION 0x01

typedef struct {
    bool help_dismissed;
} DualBootConfig;

/** Where the user drops firmware images. */
#define DUALBOOT_FW_DIR EXT_PATH("firmware")

#define DUALBOOT_MAX_FIRMWARES 32
#define DUALBOOT_NAME_LEN      64
#define DUALBOOT_MSG_LEN       96
#define DUALBOOT_CHUNK         4096

/* Rows that fit under the title on the 128x64 logical canvas. */
#define DUALBOOT_VISIBLE 4
/* Boot now / Stay here / Back to Flipper? */
#define DUALBOOT_DONE_CHOICES 3

typedef enum {
    DualBootStateList,
    DualBootStateFlashing,
    DualBootStateDone,
    DualBootStateError,
    DualBootStateHelp,
} DualBootState;

typedef enum {
    DualBootEventKey,
    DualBootEventRedraw,
    DualBootEventFinished,
} DualBootEventType;

typedef struct {
    DualBootEventType type;
    InputEvent input;
} DualBootEvent;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;

    DualBootState state;
    /* State to return to when leaving the help screen. */
    DualBootState help_return;
    /* 0 = Close, 1 = Don't show again. */
    uint8_t help_choice;

    /* List of *.bin found on the card. */
    char names[DUALBOOT_MAX_FIRMWARES][DUALBOOT_NAME_LEN];
    size_t count;
    size_t idx;
    size_t top;

    /* Flashing progress, written by the worker and read by the renderer. */
    float progress;
    char message[DUALBOOT_MSG_LEN];

    /* Done screen: 0 = Boot now, 1 = Stay here. */
    uint8_t done_idx;

    FuriThread* worker;
    bool running;
} DualBoot;

/* ------------------------------------------------------------------ */
/* Partition helpers                                                   */
/* ------------------------------------------------------------------ */

/** The second firmware's slot, or NULL on a single-app build. */
static const esp_partition_t* dualboot_target_partition(void) {
    const esp_partition_t* ota_0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);

    if(ota_0 && ota_0 == esp_ota_get_running_partition()) return NULL;
    return ota_0;
}

/* ------------------------------------------------------------------ */
/* Scanning the card                                                   */
/* ------------------------------------------------------------------ */

static bool dualboot_name_is_bin(const char* name) {
    const char* dot = strrchr(name, '.');
    if(!dot) return false;
    return (strcasecmp(dot, ".bin") == 0);
}

/** Fill the list from DUALBOOT_FW_DIR. Returns false if the folder is missing. */
static bool dualboot_scan(DualBoot* app, Storage* storage) {
    app->count = 0;
    app->idx = 0;
    app->top = 0;

    File* dir = storage_file_alloc(storage);
    bool opened = storage_dir_open(dir, DUALBOOT_FW_DIR);

    if(opened) {
        FileInfo info;
        char name[DUALBOOT_NAME_LEN];

        while(app->count < DUALBOOT_MAX_FIRMWARES &&
              storage_dir_read(dir, &info, name, sizeof(name))) {
            if(file_info_is_dir(&info)) continue;
            if(!dualboot_name_is_bin(name)) continue;

            strlcpy(app->names[app->count], name, DUALBOOT_NAME_LEN);
            app->count++;
        }
    }

    storage_dir_close(dir);
    storage_file_free(dir);
    return opened;
}

/* ------------------------------------------------------------------ */
/* The install worker                                                  */
/* ------------------------------------------------------------------ */

/* Partition table as laid down at 0x8000 of a full flash image. */
#define DUALBOOT_PART_TABLE_OFFSET 0x8000
#define DUALBOOT_PART_TABLE_MAGIC  0x50AAU
#define DUALBOOT_PART_TYPE_APP     0x00
#define DUALBOOT_PART_ENTRY_MAX    95

/** A 32-byte partition table entry, as stored in flash. */
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t type;
    uint8_t subtype;
    uint32_t offset;
    uint32_t size;
    uint8_t label[16];
    uint32_t flags;
} DualBootPartEntry;

/** Where the app image lives inside the chosen file. */
typedef struct {
    uint64_t offset;
    uint64_t length;
    bool extracted;
} DualBootImage;

/**
 * Find the app image inside the file, before writing a single byte.
 *
 * Two shapes turn up in the wild. A raw app bin starts with the image header
 * and is written as-is. A *merged* or *factory* image -- what web flashers and
 * most "download the firmware" links hand you -- starts with the bootloader and
 * carries a partition table at 0x8000; the app sits further in, at whatever
 * offset that table names. Both begin with the same 0xE9 magic, so the table is
 * the only reliable way to tell them apart.
 *
 * Rather than reject a merged image, read its table and install the app out of
 * it. Doing this up front also means a wrong file costs nothing: esp_ota_write()
 * only buffers the first sector and esp_ota_end() is what finally rejects a bad
 * header, so without this the slot is erased and fully rewritten before the
 * user is told "not valid" with no clue why.
 *
 * @return NULL when usable (and @p out filled), else a short reason to show.
 */
static const char* dualboot_locate_image(File* file, uint64_t size, DualBootImage* out) {
    esp_image_header_t header;

    out->offset = 0;
    out->length = size;
    out->extracted = false;

    if(!storage_file_seek(file, 0, true)) return "Cannot read file";
    if(storage_file_read(file, &header, sizeof(header)) != sizeof(header)) {
        return "File too small";
    }
    if(header.magic != ESP_IMAGE_HEADER_MAGIC) return "Not a firmware image";

    /* Look for an embedded partition table; absent one, this is a raw app. */
    if(size > DUALBOOT_PART_TABLE_OFFSET + sizeof(DualBootPartEntry)) {
        if(!storage_file_seek(file, DUALBOOT_PART_TABLE_OFFSET, true)) return "Cannot read file";

        for(size_t i = 0; i < DUALBOOT_PART_ENTRY_MAX; i++) {
            DualBootPartEntry entry;
            if(storage_file_read(file, &entry, sizeof(entry)) != sizeof(entry)) break;
            if(entry.magic != DUALBOOT_PART_TABLE_MAGIC) break; // end of table

            if(entry.type == DUALBOOT_PART_TYPE_APP && entry.offset < size) {
                out->offset = entry.offset;
                /* Cap at the slot the table declares: reading to EOF would drag
                 * in whatever follows, and an image carrying two app partitions
                 * would otherwise look far bigger than the app itself. Trailing
                 * padding is harmless -- esp_ota_end() validates against the
                 * length in the image header, not the byte count written. */
                out->length = size - entry.offset;
                if(out->length > entry.size) out->length = entry.size;
                out->extracted = true;
                break;
            }
        }
    }

    /* Re-validate at the real start; for a merged image the header checked
     * above was the bootloader's, not the app's. */
    if(out->extracted) {
        if(!storage_file_seek(file, (uint32_t)out->offset, true)) return "Cannot read file";
        if(storage_file_read(file, &header, sizeof(header)) != sizeof(header)) {
            return "Truncated image";
        }
        if(header.magic != ESP_IMAGE_HEADER_MAGIC) return "No app inside image";
    }

    if(header.chip_id != ESP_CHIP_ID_ESP32S3) return "Built for another chip";

    if(!storage_file_seek(file, (uint32_t)out->offset, true)) return "Cannot read file";
    return NULL;
}

static void dualboot_fail(DualBoot* app, const char* text) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    strlcpy(app->message, text, DUALBOOT_MSG_LEN);
    app->state = DualBootStateError;
    furi_mutex_release(app->mutex);
}

static int32_t dualboot_worker(void* context) {
    DualBoot* app = context;

    char path[DUALBOOT_NAME_LEN + sizeof(DUALBOOT_FW_DIR) + 2];
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    snprintf(path, sizeof(path), "%s/%s", DUALBOOT_FW_DIR, app->names[app->idx]);
    furi_mutex_release(app->mutex);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    esp_ota_handle_t handle = 0;
    bool ota_open = false;

    do {
        if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            dualboot_fail(app, "Cannot open file");
            break;
        }

        const uint64_t size = storage_file_size(file);
        const esp_partition_t* target = dualboot_target_partition();

        if(!target) {
            dualboot_fail(app, "No ota_0 slot");
            break;
        }
        if(size == 0) {
            dualboot_fail(app, "Empty file");
            break;
        }

        /* Located up front so a wrong file costs nothing: the slot is still
         * intact and whatever was installed still boots. */
        DualBootImage image;
        const char* reason = dualboot_locate_image(file, size, &image);
        if(reason) {
            dualboot_fail(app, reason);
            FURI_LOG_E(TAG, "Rejected %s: %s", path, reason);
            break;
        }
        if(image.extracted) {
            FURI_LOG_I(
                TAG,
                "Full flash image: taking app at 0x%lx (%lu KiB)",
                (unsigned long)image.offset,
                (unsigned long)(image.length / 1024));
        }

        if(image.length > target->size) {
            dualboot_fail(app, "Image too large for slot");
            break;
        }

        /* Passing the real size erases only what is needed rather than the
         * whole slot, which is the difference between a few seconds and most
         * of a minute before the first byte lands. */
        esp_err_t err = esp_ota_begin(target, (size_t)image.length, &handle);
        if(err != ESP_OK) {
            dualboot_fail(app, "Cannot start install");
            FURI_LOG_E(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
            break;
        }
        ota_open = true;

        uint8_t* buffer = malloc(DUALBOOT_CHUNK);
        uint64_t written = 0;
        bool failed = false;

        while(written < image.length) {
            const size_t want = (image.length - written) < DUALBOOT_CHUNK ?
                                    (size_t)(image.length - written) :
                                    DUALBOOT_CHUNK;
            const size_t got = storage_file_read(file, buffer, want);

            if(got == 0) {
                dualboot_fail(app, "Read error");
                failed = true;
                break;
            }

            err = esp_ota_write(handle, buffer, got);
            if(err != ESP_OK) {
                dualboot_fail(app, "Write error");
                FURI_LOG_E(TAG, "esp_ota_write: %s", esp_err_to_name(err));
                failed = true;
                break;
            }

            written += got;

            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->progress = (float)((double)written / (double)image.length);
            snprintf(
                app->message,
                DUALBOOT_MSG_LEN,
                "%lu / %lu KiB",
                (unsigned long)(written / 1024),
                (unsigned long)(image.length / 1024));
            furi_mutex_release(app->mutex);
            view_port_update(app->view_port);
        }

        free(buffer);
        if(failed) break;

        /* Validates the image header; a bad bin is rejected here rather than
         * at the next boot. */
        err = esp_ota_end(handle);
        ota_open = false;
        if(err != ESP_OK) {
            dualboot_fail(
                app,
                err == ESP_ERR_OTA_VALIDATE_FAILED ? "Not a valid firmware" : "Install failed");
            FURI_LOG_E(TAG, "esp_ota_end: %s", esp_err_to_name(err));
            break;
        }

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->state = DualBootStateDone;
        app->done_idx = 0;
        app->progress = 1.0f;
        furi_mutex_release(app->mutex);
    } while(false);

    if(ota_open) esp_ota_abort(handle);

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    DualBootEvent event = {.type = DualBootEventFinished};
    furi_message_queue_put(app->queue, &event, FuriWaitForever);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void dualboot_render_list(Canvas* canvas, DualBoot* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignCenter, "Dual Boot");

    canvas_set_font(canvas, FontSecondary);

    if(app->count == 0) {
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "No firmware found");
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Put .bin files in");
        canvas_draw_str_aligned(canvas, 64, 51, AlignCenter, AlignCenter, "SD:/firmware");
        canvas_draw_str_aligned(
            canvas, 64, 62, AlignCenter, AlignCenter, "Hold OK: back to Flipper");
        return;
    }

    for(size_t row = 0; row < DUALBOOT_VISIBLE; row++) {
        const size_t i = app->top + row;
        if(i >= app->count) break;

        const uint8_t y = 20 + (uint8_t)(row * 12);
        canvas_draw_str_aligned(canvas, 64, y, AlignCenter, AlignCenter, app->names[i]);

        if(i == app->idx) {
            elements_frame(canvas, 2, y - 6, 124, 12);
        }
    }
}

static void dualboot_render_flashing(Canvas* canvas, DualBoot* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 12, AlignCenter, AlignCenter, "Updating");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter, app->names[app->idx]);

    elements_progress_bar(canvas, 8, 36, 112, app->progress);

    canvas_draw_str_aligned(canvas, 64, 56, AlignCenter, AlignCenter, app->message);
}

/* The escape hatch, spelled out. A firmware installed here may have no way
 * back to Flipper at all (that is the whole reason the bootloader-level
 * hatch exists), so the steps have to be readable from the device itself
 * rather than only from documentation the user does not have to hand. */
static void dualboot_render_help(Canvas* canvas, DualBoot* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignCenter, "Back to Flipper");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 20, "No exit in a firmware?");
    canvas_draw_str(canvas, 2, 31, "1. Press RESET");
    canvas_draw_str(canvas, 2, 41, "2. Screen black: hold");
    canvas_draw_str(canvas, 2, 50, "   BACK ~3s");

    static const char* const choices[2] = {"Close", "Don't show again"};
    canvas_draw_str_aligned(
        canvas, 64, 59, AlignCenter, AlignCenter, choices[app->help_choice ? 1 : 0]);
    elements_frame(canvas, 12, 53, 104, 11);
}

static void dualboot_render_done(Canvas* canvas, DualBoot* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignCenter, "Installed");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignCenter, app->names[app->idx]);

    /* "Back to Flipper" sits here on purpose: this is the moment the user is
     * about to boot something that may not be able to come back on its own. */
    static const char* const choices[DUALBOOT_DONE_CHOICES] = {
        "Boot now", "Stay here", "Back to Flipper?"};
    for(uint8_t i = 0; i < DUALBOOT_DONE_CHOICES; i++) {
        const uint8_t y = 36 + (uint8_t)(i * 11);
        canvas_draw_str_aligned(canvas, 64, y, AlignCenter, AlignCenter, choices[i]);
        if(app->done_idx == i) {
            elements_frame(canvas, 12, y - 5, 104, 11);
        }
    }
}

static void dualboot_render_error(Canvas* canvas, DualBoot* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 16, AlignCenter, AlignCenter, "Install failed");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, app->message);
    canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignCenter, "Press Back");
}

static void dualboot_render_callback(Canvas* canvas, void* ctx) {
    DualBoot* app = ctx;
    if(furi_mutex_acquire(app->mutex, 200) != FuriStatusOk) return;

    switch(app->state) {
    case DualBootStateList:
        dualboot_render_list(canvas, app);
        break;
    case DualBootStateFlashing:
        dualboot_render_flashing(canvas, app);
        break;
    case DualBootStateDone:
        dualboot_render_done(canvas, app);
        break;
    case DualBootStateError:
        dualboot_render_error(canvas, app);
        break;
    case DualBootStateHelp:
        dualboot_render_help(canvas, app);
        break;
    }

    furi_mutex_release(app->mutex);
}

static void dualboot_input_callback(InputEvent* input_event, void* ctx) {
    DualBoot* app = ctx;
    DualBootEvent event = {.type = DualBootEventKey, .input = *input_event};
    furi_message_queue_put(app->queue, &event, FuriWaitForever);
}

/* ------------------------------------------------------------------ */
/* Input handling                                                      */
/* ------------------------------------------------------------------ */

static void dualboot_list_scroll_to(DualBoot* app) {
    if(app->idx < app->top) {
        app->top = app->idx;
    } else if(app->idx >= app->top + DUALBOOT_VISIBLE) {
        app->top = app->idx - DUALBOOT_VISIBLE + 1;
    }
}

/** @return false when the app should exit. */
static bool dualboot_handle_key(DualBoot* app, const InputEvent* input) {
    /* Hold OK anywhere in the list to read the escape-hatch steps. Long is
     * filtered out below, so it has to be handled before that. */
    if(input->type == InputTypeLong && input->key == InputKeyOk &&
       app->state == DualBootStateList) {
        app->help_return = app->state;
        app->state = DualBootStateHelp;
        return true;
    }

    if(input->type != InputTypeShort && input->type != InputTypeRepeat) return true;

    switch(app->state) {
    case DualBootStateList:
        if(input->key == InputKeyBack) return false;

        if(app->count == 0) break;

        if(input->key == InputKeyUp) {
            app->idx = (app->idx == 0) ? app->count - 1 : app->idx - 1;
            dualboot_list_scroll_to(app);
        } else if(input->key == InputKeyDown) {
            app->idx = (app->idx + 1 >= app->count) ? 0 : app->idx + 1;
            dualboot_list_scroll_to(app);
        } else if(input->key == InputKeyOk) {
            app->state = DualBootStateFlashing;
            app->progress = 0.0f;
            strlcpy(app->message, "Preparing...", DUALBOOT_MSG_LEN);

            app->worker = furi_thread_alloc_ex("DualBootWorker", 4096, dualboot_worker, app);
            furi_thread_start(app->worker);
        }
        break;

    case DualBootStateFlashing:
        /* Back is ignored on purpose: aborting mid-write would leave a partial
         * image in the slot. It cannot brick anything -- the bootloader falls
         * back to factory -- but a silent half-install is worse than waiting. */
        break;

    case DualBootStateDone:
        if(input->key == InputKeyBack) return false;

        if(input->key == InputKeyUp) {
            app->done_idx =
                (app->done_idx == 0) ? DUALBOOT_DONE_CHOICES - 1 : app->done_idx - 1;
        } else if(input->key == InputKeyDown) {
            app->done_idx = (app->done_idx + 1 >= DUALBOOT_DONE_CHOICES) ?
                                0 :
                                app->done_idx + 1;
        } else if(input->key == InputKeyOk && app->done_idx == 2) {
            app->help_return = app->state;
            app->state = DualBootStateHelp;
        } else if(input->key == InputKeyOk) {
            if(app->done_idx == 0) {
                const esp_partition_t* target = dualboot_target_partition();
                esp_err_t err =
                    target ? esp_ota_set_boot_partition(target) : ESP_ERR_NOT_FOUND;

                if(err == ESP_OK) {
                    FURI_LOG_I(TAG, "Booting ota_0");
                    esp_restart();
                }

                strlcpy(app->message, "Cannot select slot", DUALBOOT_MSG_LEN);
                app->state = DualBootStateError;
                FURI_LOG_E(TAG, "set_boot_partition: %s", esp_err_to_name(err));
            } else {
                return false;
            }
        }
        break;

    case DualBootStateError:
        if(input->key == InputKeyBack || input->key == InputKeyOk) return false;
        break;

    case DualBootStateHelp:
        if(input->key == InputKeyUp || input->key == InputKeyDown) {
            app->help_choice = app->help_choice ? 0 : 1;
        } else if(input->key == InputKeyOk) {
            if(app->help_choice) {
                const DualBootConfig cfg = {.help_dismissed = true};
                saved_struct_save(
                    DUALBOOT_CFG_PATH,
                    &cfg,
                    sizeof(cfg),
                    DUALBOOT_CFG_MAGIC,
                    DUALBOOT_CFG_VERSION);
            }
            app->state = app->help_return;
        } else if(input->key == InputKeyBack) {
            /* Back is Close: never silently opts out. */
            app->state = app->help_return;
        }
        break;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int32_t dualboot_app(void* p) {
    UNUSED(p);

    DualBoot* app = malloc(sizeof(DualBoot));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(DualBootEvent));
    /* First run shows the escape hatch unprompted: a firmware installed here
     * may have no way back, and by then this screen is unreachable. */
    DualBootConfig cfg = {.help_dismissed = false};
    saved_struct_load(
        DUALBOOT_CFG_PATH, &cfg, sizeof(cfg), DUALBOOT_CFG_MAGIC, DUALBOOT_CFG_VERSION);

    app->help_choice = 0;
    app->help_return = DualBootStateList;
    app->state = cfg.help_dismissed ? DualBootStateList : DualBootStateHelp;
    app->running = true;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!dualboot_scan(app, storage)) {
        /* Create it rather than just reporting it missing, so the folder the
         * on-screen hint names actually exists for the user to drop bins into. */
        if(storage_simply_mkdir(storage, DUALBOOT_FW_DIR)) {
            FURI_LOG_I(TAG, "Created %s", DUALBOOT_FW_DIR);
        } else {
            FURI_LOG_W(TAG, "Cannot create %s (no SD card?)", DUALBOOT_FW_DIR);
        }
    }
    furi_record_close(RECORD_STORAGE);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, dualboot_render_callback, app);
    view_port_input_callback_set(app->view_port, dualboot_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    DualBootEvent event;
    while(app->running) {
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) != FuriStatusOk) continue;

        furi_mutex_acquire(app->mutex, FuriWaitForever);

        if(event.type == DualBootEventKey) {
            app->running = dualboot_handle_key(app, &event.input);
        } else if(event.type == DualBootEventFinished) {
            /* The worker has already set Done or Error. */
            furi_thread_join(app->worker);
            furi_thread_free(app->worker);
            app->worker = NULL;
        }

        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
    }

    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
