/**
 * @file voice_notes.c
 * @brief Record voice notes to the SD card and play them back.
 *
 * Records mono 16-bit PCM at the speaker's own rate, deliberately: playing back
 * at a different rate than the I2S clock is configured for would mean
 * reconfiguring the clock on every play, and the tone paths would then have to
 * put it back. Same rate both ways keeps it simple and keeps pitch correct.
 *
 * Files are plain WAV so they can be pulled off the card and opened anywhere,
 * rather than a private format only this app understands.
 */

#include <furi.h>
#include <furi_hal_mic.h>
#include <furi_hal_speaker.h>
#include <furi_hal_power.h>
#include <furi_hal_rtc.h>

#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <storage/storage.h>

#include <stdlib.h>
#include <string.h>

#define TAG "VoiceNotes"

#define VN_DIR  EXT_PATH("voice_notes")
#define VN_RATE 44100u

/* Read/write in blocks; big enough to keep the card happy, small enough that
 * stopping feels immediate. */
#define VN_CHUNK 1024

#define VN_MAX_FILES 32
#define VN_NAME_LEN  40

typedef enum {
    VnStateBrowse,
    VnStateRecording,
    VnStatePlaying,
} VnState;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;

    VnState state;
    bool mic_ok;
    bool have_speaker;

    char names[VN_MAX_FILES][VN_NAME_LEN];
    size_t count;
    size_t idx;
    size_t top;

    uint32_t elapsed_ms;
    char status[48];

    int16_t* chunk;
    volatile bool worker_stop;
    FuriThread* worker;
} VoiceNotes;

/* ------------------------------------------------------------------ */
/* WAV                                                                 */
/* ------------------------------------------------------------------ */

/** Minimal 44-byte canonical WAV header, mono 16-bit. */
static void vn_wav_header(uint8_t* h, uint32_t rate, uint32_t data_bytes) {
    const uint32_t byte_rate = rate * 2;
    memcpy(h, "RIFF", 4);
    const uint32_t riff = 36 + data_bytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t fmt_len = 16;
    memcpy(h + 16, &fmt_len, 4);
    const uint16_t pcm = 1, ch = 1, bits = 16, align = 2;
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
}

static void vn_scan(VoiceNotes* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, VN_DIR);

    app->count = 0;
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, VN_DIR)) {
        FileInfo info;
        char name[VN_NAME_LEN];
        while(app->count < VN_MAX_FILES && storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            strlcpy(app->names[app->count], name, VN_NAME_LEN);
            app->count++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);

    if(app->idx >= app->count) app->idx = app->count ? app->count - 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Workers                                                             */
/* ------------------------------------------------------------------ */

static int32_t vn_record_worker(void* context) {
    VoiceNotes* app = context;

    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char path[128];
    snprintf(
        path, sizeof(path), "%s/note_%02u%02u%02u_%02u%02u%02u.wav", VN_DIR, dt.year % 100,
        dt.month, dt.day, dt.hour, dt.minute, dt.second);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    uint32_t data_bytes = 0;
    bool ok = storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        uint8_t header[44];
        vn_wav_header(header, VN_RATE, 0); /* patched on the way out */
        storage_file_write(file, header, sizeof(header));

        while(!app->worker_stop) {
            const size_t got = furi_hal_mic_read(app->chunk, VN_CHUNK, 300);
            if(got == 0) continue;
            const uint16_t bytes = (uint16_t)(got * sizeof(int16_t));
            if(storage_file_write(file, app->chunk, bytes) != bytes) break;
            data_bytes += bytes;

            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->elapsed_ms = (data_bytes / 2) * 1000 / VN_RATE;
            furi_mutex_release(app->mutex);
        }

        /* Now that the length is known, rewrite the two size fields. */
        uint8_t header2[44];
        vn_wav_header(header2, VN_RATE, data_bytes);
        storage_file_seek(file, 0, true);
        storage_file_write(file, header2, sizeof(header2));
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    snprintf(
        app->status, sizeof(app->status), ok ? "saved %lus" : "record failed",
        (unsigned long)(data_bytes / 2 / VN_RATE));
    app->state = VnStateBrowse;
    furi_mutex_release(app->mutex);

    return 0;
}

static int32_t vn_play_worker(void* context) {
    VoiceNotes* app = context;

    char path[128];
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    snprintf(path, sizeof(path), "%s/%s", VN_DIR, app->names[app->idx]);
    furi_mutex_release(app->mutex);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_seek(file, 44, true); /* skip the header */
        furi_hal_speaker_pcm_start();

        uint32_t played = 0;
        while(!app->worker_stop) {
            const uint16_t want = VN_CHUNK * sizeof(int16_t);
            const uint16_t got = storage_file_read(file, app->chunk, want);
            if(got == 0) break;

            const size_t samples = got / sizeof(int16_t);
            furi_hal_speaker_pcm_write(app->chunk, samples, 500);
            played += samples;

            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->elapsed_ms = played * 1000 / VN_RATE;
            furi_mutex_release(app->mutex);
        }
        furi_hal_speaker_pcm_stop();
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->state = VnStateBrowse;
    strlcpy(app->status, "done", sizeof(app->status));
    furi_mutex_release(app->mutex);

    return 0;
}

static void vn_worker_start(VoiceNotes* app, FuriThreadCallback cb, const char* name) {
    app->worker_stop = false;
    app->worker = furi_thread_alloc_ex(name, 4096, cb, app);
    furi_thread_start(app->worker);
}

static void vn_worker_join(VoiceNotes* app) {
    if(!app->worker) return;
    app->worker_stop = true;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    app->worker = NULL;
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static void vn_draw_callback(Canvas* canvas, void* ctx) {
    VoiceNotes* app = ctx;
    if(furi_mutex_acquire(app->mutex, 50) != FuriStatusOk) return;

    char line[48];
    canvas_set_font(canvas, FontPrimary);

    if(app->state == VnStateRecording) {
        canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, "Recording");
        canvas_set_font(canvas, FontBigNumbers);
        snprintf(line, sizeof(line), "%lu", (unsigned long)(app->elapsed_ms / 1000));
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, line);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignCenter, "OK or Back: stop");
        furi_mutex_release(app->mutex);
        return;
    }

    if(app->state == VnStatePlaying) {
        canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, "Playing");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, app->names[app->idx]);
        snprintf(line, sizeof(line), "%lu s", (unsigned long)(app->elapsed_ms / 1000));
        canvas_draw_str_aligned(canvas, 64, 46, AlignCenter, AlignCenter, line);
        canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignCenter, "OK or Back: stop");
        furi_mutex_release(app->mutex);
        return;
    }

    canvas_draw_str(canvas, 2, 9, "Voice Notes");
    canvas_set_font(canvas, FontSecondary);

    if(!app->mic_ok) {
        canvas_draw_str(canvas, 2, 22, "Mic failed to start");
    } else if(app->count == 0) {
        canvas_draw_str(canvas, 2, 22, "No notes yet");
        canvas_draw_str(canvas, 2, 34, "Hold OK to record");
    } else {
        for(size_t r = 0; r < 3; r++) {
            const size_t i = app->top + r;
            if(i >= app->count) break;
            const int y = 21 + (int)r * 11;
            canvas_draw_str(canvas, 6, y, app->names[i]);
            if(i == app->idx) elements_frame(canvas, 2, y - 9, 124, 11);
        }
    }

    if(app->status[0]) {
        canvas_draw_str(canvas, 2, 55, app->status);
    }
    canvas_draw_str(canvas, 2, 64, "OK: play   hold OK: record");

    furi_mutex_release(app->mutex);
}

static void vn_input_callback(InputEvent* event, void* ctx) {
    VoiceNotes* app = ctx;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t voice_notes_app(void* p) {
    UNUSED(p);

    VoiceNotes* app = malloc(sizeof(VoiceNotes));
    memset(app, 0, sizeof(VoiceNotes));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->chunk = malloc(VN_CHUNK * sizeof(int16_t));

    vn_scan(app);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, vn_draw_callback, app);
    view_port_input_callback_set(app->view_port, vn_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    /* Sleeping mid-recording would truncate the file. */
    furi_hal_power_insomnia_enter();

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->queue, &event, 100) == FuriStatusOk) {
            if(event.type == InputTypeShort || event.type == InputTypeLong) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                const VnState state = app->state;
                furi_mutex_release(app->mutex);

                if(state == VnStateRecording || state == VnStatePlaying) {
                    if(event.key == InputKeyOk || event.key == InputKeyBack) {
                        vn_worker_join(app);
                        furi_hal_mic_stop();
                        if(app->have_speaker) {
                            furi_hal_speaker_release();
                            app->have_speaker = false;
                        }
                        vn_scan(app);
                    }
                } else if(event.key == InputKeyBack) {
                    running = false;
                } else if(event.key == InputKeyOk && event.type == InputTypeLong) {
                    app->mic_ok = furi_hal_mic_start(VN_RATE);
                    if(app->mic_ok) {
                        furi_mutex_acquire(app->mutex, FuriWaitForever);
                        app->state = VnStateRecording;
                        app->elapsed_ms = 0;
                        app->status[0] = '\0';
                        furi_mutex_release(app->mutex);
                        vn_worker_start(app, vn_record_worker, "VnRec");
                    }
                } else if(event.key == InputKeyOk && app->count > 0) {
                    app->have_speaker = furi_hal_speaker_acquire(1000);
                    if(app->have_speaker) {
                        furi_mutex_acquire(app->mutex, FuriWaitForever);
                        app->state = VnStatePlaying;
                        app->elapsed_ms = 0;
                        furi_mutex_release(app->mutex);
                        vn_worker_start(app, vn_play_worker, "VnPlay");
                    } else {
                        strlcpy(app->status, "speaker busy", sizeof(app->status));
                    }
                } else if(event.key == InputKeyUp || event.key == InputKeyDown) {
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    if(app->count > 0) {
                        if(event.key == InputKeyUp) {
                            app->idx = (app->idx == 0) ? app->count - 1 : app->idx - 1;
                        } else {
                            app->idx = (app->idx + 1 >= app->count) ? 0 : app->idx + 1;
                        }
                        if(app->idx < app->top) app->top = app->idx;
                        if(app->idx >= app->top + 3) app->top = app->idx - 2;
                    }
                    furi_mutex_release(app->mutex);
                }
            }
        }
        view_port_update(app->view_port);
    }

    vn_worker_join(app);
    furi_hal_mic_stop();
    if(app->have_speaker) furi_hal_speaker_release();
    furi_hal_power_insomnia_exit();

    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    free(app->chunk);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
