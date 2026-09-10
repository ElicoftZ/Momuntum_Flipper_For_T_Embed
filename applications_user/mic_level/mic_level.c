/**
 * @file mic_level.c
 * @brief Sound level meter for the PDM microphone.
 *
 * Honest about what it is: the reading is dBFS -- decibels relative to the
 * digital full scale of this microphone -- not SPL. Turning it into real dB SPL
 * needs a calibrated reference the board does not have, so the app shows the
 * raw figure plus an offset you can set yourself against a known meter, rather
 * than inventing a number and calling it dB.
 */

#include <furi.h>
#include <furi_hal_mic.h>

#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>

#include "../mic_common/mic_fft.h"

#define TAG "MicLevel"

/* Bar spans this range; below the floor the mic is only hearing itself. */
#define LVL_FLOOR_DB -90.0f
#define LVL_TOP_DB   0.0f

#define LVL_HISTORY 100

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;

    MicFft fft;
    float mag[MIC_BINS];

    float db;
    float min_db;
    float max_db;
    /* Added to the displayed figure so a user with a reference meter can make
     * this read SPL. Persisting it would imply a calibration we cannot verify. */
    float offset_db;

    float history[LVL_HISTORY];
    size_t hist_head;
    size_t hist_count;

    bool running;
} MicLevel;

static void lvl_draw_callback(Canvas* canvas, void* ctx) {
    MicLevel* app = ctx;
    if(furi_mutex_acquire(app->mutex, 50) != FuriStatusOk) return;

    if(!app->running) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "Mic failed to start");
        furi_mutex_release(app->mutex);
        return;
    }

    char line[40];

    /* The number, big. */
    canvas_set_font(canvas, FontBigNumbers);
    snprintf(line, sizeof(line), "%d", (int)(app->db + app->offset_db));
    canvas_draw_str_aligned(canvas, 42, 16, AlignCenter, AlignCenter, line);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 62, 12, app->offset_db != 0.0f ? "dB" : "dBFS");
    snprintf(
        line,
        sizeof(line),
        "min %d  max %d",
        (int)(app->min_db + app->offset_db),
        (int)(app->max_db + app->offset_db));
    canvas_draw_str(canvas, 62, 22, line);

    /* Level bar. */
    const float span = LVL_TOP_DB - LVL_FLOOR_DB;
    float norm = (app->db - LVL_FLOOR_DB) / span;
    if(norm < 0.0f) norm = 0.0f;
    if(norm > 1.0f) norm = 1.0f;
    canvas_draw_frame(canvas, 2, 26, 124, 8);
    canvas_draw_box(canvas, 3, 27, (int)(norm * 122.0f), 6);

    /* Recent history, so a transient is visible after it has passed. */
    for(size_t i = 0; i < app->hist_count; i++) {
        const size_t idx = (app->hist_head + LVL_HISTORY - app->hist_count + i) % LVL_HISTORY;
        float n = (app->history[idx] - LVL_FLOOR_DB) / span;
        if(n < 0.0f) n = 0.0f;
        if(n > 1.0f) n = 1.0f;
        const int h = (int)(n * 18.0f);
        if(h > 0) canvas_draw_line(canvas, (int)i + 2, 56, (int)i + 2, 56 - h);
    }
    canvas_draw_line(canvas, 0, 57, 127, 57);

    if(app->offset_db != 0.0f) {
        snprintf(line, sizeof(line), "cal %+d  OK reset", (int)app->offset_db);
    } else {
        snprintf(line, sizeof(line), "wheel: calibrate  OK: reset");
    }
    canvas_draw_str(canvas, 2, 64, line);

    furi_mutex_release(app->mutex);
}

static void lvl_input_callback(InputEvent* event, void* ctx) {
    MicLevel* app = ctx;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t mic_level_app(void* p) {
    UNUSED(p);

    MicLevel* app = malloc(sizeof(MicLevel));
    memset(app, 0, sizeof(MicLevel));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->min_db = 0.0f;
    app->max_db = -120.0f;

    if(!mic_fft_alloc(&app->fft)) FURI_LOG_E(TAG, "out of memory");
    app->running = furi_hal_mic_start(MIC_SAMPLE_RATE);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, lvl_draw_callback, app);
    view_port_input_callback_set(app->view_port, lvl_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    bool running = true;
    while(running) {
        InputEvent event;
        while(furi_message_queue_get(app->queue, &event, 0) == FuriStatusOk) {
            if(event.type != InputTypeShort) continue;
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(event.key == InputKeyBack) {
                running = false;
            } else if(event.key == InputKeyOk) {
                app->min_db = 0.0f;
                app->max_db = -120.0f;
                app->hist_count = 0;
                app->hist_head = 0;
            } else if(event.key == InputKeyUp) {
                app->offset_db += 1.0f;
            } else if(event.key == InputKeyDown) {
                app->offset_db -= 1.0f;
            }
            furi_mutex_release(app->mutex);
        }

        if(app->running) {
            size_t peak = 0;
            if(mic_fft_capture(&app->fft, app->mag, &peak)) {
                const float db = mic_fft_level_db(&app->fft);

                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->db = db;
                if(db < app->min_db) app->min_db = db;
                if(db > app->max_db) app->max_db = db;

                app->history[app->hist_head] = db;
                app->hist_head = (app->hist_head + 1) % LVL_HISTORY;
                if(app->hist_count < LVL_HISTORY) app->hist_count++;
                furi_mutex_release(app->mutex);
            }
        } else {
            furi_delay_ms(100);
        }
        view_port_update(app->view_port);
    }

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
