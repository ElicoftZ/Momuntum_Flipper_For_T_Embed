/**
 * @file mic_waterfall.c
 * @brief Scrolling spectrogram of the PDM microphone.
 *
 * The plain spectrum view only shows *now*, so a beacon that pulses every few
 * seconds is invisible unless you happen to be looking at the right instant --
 * which is exactly how ad-tech ultrasonic beacons behave. A waterfall keeps the
 * last few seconds on screen, so an intermittent tone reads as a dotted vertical
 * stripe instead of a flicker you missed.
 */

#include <furi.h>
#include <furi_hal_mic.h>

#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>

#include "../mic_common/mic_fft.h"

#define TAG "MicWaterfall"

#define WF_TOP  12
#define WF_BOT  56
#define WF_ROWS (WF_BOT - WF_TOP)

/* Threshold for painting a pixel. Below this the display is just noise texture. */
#define WF_INK 0.22f

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;

    MicFft fft;
    float mag[MIC_BINS];

    /* One bit per bin per row, newest at row 0. 44 rows x 128 bins packed to
     * bytes: small enough to live comfortably in internal RAM. */
    uint8_t rows[WF_ROWS][MIC_BINS / 8];
    size_t head; /* newest row index into rows[] */

    float peak_hz;
    bool paused;
    bool running;
} Waterfall;

static inline bool wf_get(Waterfall* app, size_t row, size_t bin) {
    return (app->rows[row][bin >> 3] >> (bin & 7)) & 1u;
}

static void wf_push_row(Waterfall* app) {
    /* Ring buffer: move the head back rather than shifting 5 KB every frame. */
    app->head = (app->head + WF_ROWS - 1) % WF_ROWS;
    uint8_t* row = app->rows[app->head];
    memset(row, 0, MIC_BINS / 8);

    for(size_t i = 0; i < MIC_BINS; i++) {
        if(app->mag[i] >= WF_INK) row[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
}

static void wf_draw_callback(Canvas* canvas, void* ctx) {
    Waterfall* app = ctx;
    if(furi_mutex_acquire(app->mutex, 50) != FuriStatusOk) return;

    if(!app->running) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "Mic failed to start");
        furi_mutex_release(app->mutex);
        return;
    }

    char line[32];
    canvas_set_font(canvas, FontSecondary);

    const bool ultra = app->peak_hz >= MIC_ULTRASONIC_HZ;
    snprintf(
        line,
        sizeof(line),
        "%s%.1f kHz",
        ultra ? "ULTRA " : "peak ",
        (double)(app->peak_hz / 1000.0f));
    canvas_draw_str(canvas, 2, 9, line);
    if(app->paused) canvas_draw_str(canvas, 96, 9, "PAUSE");

    /* Newest row at the top, scrolling downward as time passes. */
    for(size_t r = 0; r < WF_ROWS; r++) {
        const size_t src = (app->head + r) % WF_ROWS;
        const int y = WF_TOP + (int)r;
        for(size_t b = 0; b < MIC_BINS; b++) {
            if(wf_get(app, src, b)) canvas_draw_dot(canvas, (int)b, y);
        }
    }

    /* Where hearing ends: anything to the right of this line is inaudible. */
    const int ultra_x = (int)(MIC_ULTRASONIC_HZ / MIC_BIN_HZ);
    for(int y = WF_TOP; y < WF_BOT; y += 4) {
        canvas_draw_dot(canvas, ultra_x, y);
    }

    canvas_draw_line(canvas, 0, WF_BOT, 127, WF_BOT);
    canvas_draw_str(canvas, 2, 63, "0");
    canvas_draw_str(canvas, ultra_x - 8, 63, "17k");
    canvas_draw_str(canvas, 108, 63, "24k");

    furi_mutex_release(app->mutex);
}

static void wf_input_callback(InputEvent* event, void* ctx) {
    Waterfall* app = ctx;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t mic_waterfall_app(void* p) {
    UNUSED(p);

    Waterfall* app = malloc(sizeof(Waterfall));
    memset(app, 0, sizeof(Waterfall));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    if(!mic_fft_alloc(&app->fft)) {
        FURI_LOG_E(TAG, "out of memory");
    }
    app->running = furi_hal_mic_start(MIC_SAMPLE_RATE);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, wf_draw_callback, app);
    view_port_input_callback_set(app->view_port, wf_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    bool running = true;
    while(running) {
        InputEvent event;
        while(furi_message_queue_get(app->queue, &event, 0) == FuriStatusOk) {
            if(event.type != InputTypeShort) continue;
            if(event.key == InputKeyBack) {
                running = false;
            } else if(event.key == InputKeyOk) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->paused = !app->paused;
                furi_mutex_release(app->mutex);
            } else if(event.key == InputKeyUp || event.key == InputKeyDown) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                memset(app->rows, 0, sizeof(app->rows));
                furi_mutex_release(app->mutex);
            }
        }

        if(app->running) {
            size_t peak_bin = 0;
            if(mic_fft_capture(&app->fft, app->mag, &peak_bin)) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->peak_hz = (float)peak_bin * MIC_BIN_HZ;
                if(!app->paused) wf_push_row(app);
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
