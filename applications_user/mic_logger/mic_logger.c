/**
 * @file mic_logger.c
 * @brief Log inaudible tones to the SD card, unattended.
 *
 * The spectrum and waterfall apps answer "is something there right now". This
 * one answers "was something there while I was not looking" -- leave it running
 * in a shop or a lobby and read the log afterwards. Ultrasonic beacons are
 * intermittent by design, so catching one is mostly a question of dwell time.
 *
 * A detection is a sustained tone above 17 kHz, not a single loud frame: a key
 * jingle or a chair scrape puts a brief spike up there and would otherwise fill
 * the log with nothing.
 */

#include <furi.h>
#include <furi_hal_mic.h>
#include <furi_hal_rtc.h>
#include <furi_hal_power.h>

#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <storage/storage.h>

#include "../mic_common/mic_fft.h"

#define TAG "MicLogger"

#define LOG_PATH EXT_PATH("apps_data/mic_logger.txt")

/* A tone has to clear this and hold for LOG_HOLD_FRAMES before it counts. At
 * ~20 frames/s that is roughly a third of a second. */
#define LOG_THRESHOLD    0.30f
#define LOG_HOLD_FRAMES  6
/* Do not re-log the same continuous tone; wait for it to stop first. */
#define LOG_REARM_FRAMES 20

#define LOG_RECENT 4

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;

    MicFft fft;
    float mag[MIC_BINS];

    bool logging;
    bool running;

    uint32_t detections;
    uint16_t hold;   /* consecutive frames above threshold */
    uint16_t quiet;  /* consecutive frames below, for re-arming */
    bool armed;

    float last_hz;
    char recent[LOG_RECENT][32];
    size_t recent_count;

    float peak_hz;
    float peak_mag;
} MicLogger;

static void logger_note(MicLogger* app, float hz) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);

    char stamp[32];
    snprintf(
        stamp, sizeof(stamp), "%02u:%02u:%02u  %.1fkHz", dt.hour, dt.minute, dt.second,
        (double)(hz / 1000.0f));

    /* Newest first, so the screen shows what just happened. */
    for(size_t i = LOG_RECENT - 1; i > 0; i--) {
        memcpy(app->recent[i], app->recent[i - 1], sizeof(app->recent[0]));
    }
    strlcpy(app->recent[0], stamp, sizeof(app->recent[0]));
    if(app->recent_count < LOG_RECENT) app->recent_count++;

    app->detections++;
    app->last_hz = hz;

    /* Append and close every time: the whole point is being able to yank the
     * card, or run out of battery, without losing what was caught. */
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char row[64];
        const int n = snprintf(
            row, sizeof(row), "%04u-%02u-%02u %s\n", dt.year, dt.month, dt.day, stamp);
        storage_file_write(file, row, (uint16_t)n);
    } else {
        FURI_LOG_E(TAG, "cannot open %s", LOG_PATH);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void logger_draw_callback(Canvas* canvas, void* ctx) {
    MicLogger* app = ctx;
    if(furi_mutex_acquire(app->mutex, 50) != FuriStatusOk) return;

    if(!app->running) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "Mic failed to start");
        furi_mutex_release(app->mutex);
        return;
    }

    char line[40];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 9, app->logging ? "Logging" : "Stopped");

    canvas_set_font(canvas, FontSecondary);
    snprintf(line, sizeof(line), "%lu found", (unsigned long)app->detections);
    canvas_draw_str(canvas, 64, 9, line);

    /* Live indicator: what the mic hears above the ultrasonic line right now. */
    if(app->peak_hz >= MIC_ULTRASONIC_HZ && app->peak_mag > 0.15f) {
        snprintf(line, sizeof(line), "now: %.1f kHz", (double)(app->peak_hz / 1000.0f));
    } else {
        snprintf(line, sizeof(line), "now: nothing above 17k");
    }
    canvas_draw_str(canvas, 2, 20, line);
    canvas_draw_line(canvas, 0, 23, 127, 23);

    for(size_t i = 0; i < app->recent_count; i++) {
        canvas_draw_str(canvas, 2, 33 + (int)i * 9, app->recent[i]);
    }
    if(app->recent_count == 0) {
        canvas_draw_str(canvas, 2, 33, "no detections yet");
    }

    canvas_draw_str(canvas, 2, 64, app->logging ? "OK: stop" : "OK: start");

    furi_mutex_release(app->mutex);
}

static void logger_input_callback(InputEvent* event, void* ctx) {
    MicLogger* app = ctx;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t mic_logger_app(void* p) {
    UNUSED(p);

    MicLogger* app = malloc(sizeof(MicLogger));
    memset(app, 0, sizeof(MicLogger));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->armed = true;

    if(!mic_fft_alloc(&app->fft)) FURI_LOG_E(TAG, "out of memory");
    app->running = furi_hal_mic_start(MIC_SAMPLE_RATE);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, logger_draw_callback, app);
    view_port_input_callback_set(app->view_port, logger_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    /* Unattended is the point: do not let the board sleep while armed. */
    furi_hal_power_insomnia_enter();

    bool running = true;
    while(running) {
        InputEvent event;
        while(furi_message_queue_get(app->queue, &event, 0) == FuriStatusOk) {
            if(event.type != InputTypeShort) continue;
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(event.key == InputKeyBack) {
                running = false;
            } else if(event.key == InputKeyOk) {
                app->logging = !app->logging;
                app->hold = 0;
                app->quiet = 0;
                app->armed = true;
            }
            furi_mutex_release(app->mutex);
        }

        if(app->running) {
            size_t peak_bin = 0;
            if(mic_fft_capture(&app->fft, app->mag, &peak_bin)) {
                const float hz = (float)peak_bin * MIC_BIN_HZ;
                const float m = app->mag[peak_bin];
                const bool hit = (hz >= MIC_ULTRASONIC_HZ) && (m >= LOG_THRESHOLD);

                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->peak_hz = hz;
                app->peak_mag = m;

                if(app->logging) {
                    if(hit) {
                        app->quiet = 0;
                        if(app->hold < 0xFFFF) app->hold++;
                        if(app->armed && app->hold >= LOG_HOLD_FRAMES) {
                            logger_note(app, hz);
                            app->armed = false;
                        }
                    } else {
                        app->hold = 0;
                        if(app->quiet < 0xFFFF) app->quiet++;
                        if(app->quiet >= LOG_REARM_FRAMES) app->armed = true;
                    }
                }
                furi_mutex_release(app->mutex);
            }
        } else {
            furi_delay_ms(100);
        }
        view_port_update(app->view_port);
    }

    furi_hal_power_insomnia_exit();
    furi_hal_mic_stop();
    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    mic_fft_free(&app->fft);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
