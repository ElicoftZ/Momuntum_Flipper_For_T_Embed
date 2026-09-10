/**
 * @file tone_gen.c
 * @brief Tone generator: wheel sets the frequency, OK plays it.
 *
 * Spans 20 Hz to 22 kHz, so it covers the whole audible range and continues
 * past it into the ultrasonic band the mic apps look at -- useful for checking
 * that a repeller or a beacon detector actually responds, and for testing your
 * own hearing limit.
 *
 * The step size scales with frequency. A fixed step is unusable across three
 * decades: 10 Hz is far too coarse at the bottom and absurdly fine at the top.
 */

#include <furi.h>
#include <furi_hal_speaker.h>

#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>

#include <stdlib.h>
#include <string.h>

#define TAG "ToneGen"

#define TONE_MIN_HZ 20.0f
#define TONE_MAX_HZ 22000.0f

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;

    float hz;
    float volume;
    bool playing;
    bool have_speaker;
} ToneGen;

/** Step that feels the same at 50 Hz and at 19 kHz. */
static float tone_step(float hz) {
    if(hz < 200.0f) return 5.0f;
    if(hz < 1000.0f) return 25.0f;
    if(hz < 5000.0f) return 100.0f;
    if(hz < 12000.0f) return 250.0f;
    return 500.0f;
}

static void tone_apply(ToneGen* app) {
    if(!app->playing) return;
    if(!app->have_speaker) {
        app->have_speaker = furi_hal_speaker_acquire(1000);
        if(!app->have_speaker) {
            FURI_LOG_E(TAG, "speaker busy");
            app->playing = false;
            return;
        }
    }
    furi_hal_speaker_start(app->hz, app->volume);
}

static void tone_draw_callback(Canvas* canvas, void* ctx) {
    ToneGen* app = ctx;
    if(furi_mutex_acquire(app->mutex, 50) != FuriStatusOk) return;

    char line[40];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Tone Generator");

    /* The frequency, big enough to read at arm's length. */
    canvas_set_font(canvas, FontBigNumbers);
    if(app->hz >= 1000.0f) {
        snprintf(line, sizeof(line), "%.2f", (double)(app->hz / 1000.0f));
    } else {
        snprintf(line, sizeof(line), "%d", (int)app->hz);
    }
    canvas_draw_str_aligned(canvas, 58, 30, AlignCenter, AlignCenter, line);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 92, 34, app->hz >= 1000.0f ? "kHz" : "Hz");

    /* Where this sits relative to hearing. */
    if(app->hz >= 17000.0f) {
        canvas_draw_str(canvas, 2, 45, "above hearing (ultrasonic)");
    } else if(app->hz <= 40.0f) {
        canvas_draw_str(canvas, 2, 45, "very low - may be inaudible");
    } else {
        snprintf(line, sizeof(line), "audible   step %d Hz", (int)tone_step(app->hz));
        canvas_draw_str(canvas, 2, 45, line);
    }

    /* Position along the range, so the wheel has somewhere to be. */
    const float pos = (app->hz - TONE_MIN_HZ) / (TONE_MAX_HZ - TONE_MIN_HZ);
    canvas_draw_frame(canvas, 2, 49, 124, 6);
    canvas_draw_box(canvas, 3, 50, (int)(pos * 122.0f), 4);

    snprintf(
        line,
        sizeof(line),
        "%s   vol %d%%   OK: %s",
        app->playing ? "PLAYING" : "silent",
        (int)(app->volume * 100.0f),
        app->playing ? "stop" : "play");
    canvas_draw_str(canvas, 2, 63, line);

    furi_mutex_release(app->mutex);
}

static void tone_input_callback(InputEvent* event, void* ctx) {
    ToneGen* app = ctx;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t tone_gen_app(void* p) {
    UNUSED(p);

    ToneGen* app = malloc(sizeof(ToneGen));
    memset(app, 0, sizeof(ToneGen));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->hz = 1000.0f;
    app->volume = 0.5f;

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, tone_draw_callback, app);
    view_port_input_callback_set(app->view_port, tone_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) != FuriStatusOk) continue;
        if(event.type != InputTypeShort && event.type != InputTypeRepeat &&
           event.type != InputTypeLong) {
            continue;
        }

        furi_mutex_acquire(app->mutex, FuriWaitForever);

        if(event.key == InputKeyBack) {
            running = false;
        } else if(event.key == InputKeyOk && event.type == InputTypeLong) {
            /* Volume in quarters; long-press keeps it off the wheel, which is
             * busy with the frequency. */
            app->volume += 0.25f;
            if(app->volume > 1.0f) app->volume = 0.25f;
            if(app->playing) furi_hal_speaker_start(app->hz, app->volume);
        } else if(event.key == InputKeyOk && event.type == InputTypeShort) {
            app->playing = !app->playing;
            if(app->playing) {
                tone_apply(app);
            } else {
                if(app->have_speaker) furi_hal_speaker_stop();
            }
        } else if(event.key == InputKeyUp || event.key == InputKeyDown) {
            const float step = tone_step(app->hz);
            app->hz += (event.key == InputKeyUp) ? step : -step;
            if(app->hz > TONE_MAX_HZ) app->hz = TONE_MAX_HZ;
            if(app->hz < TONE_MIN_HZ) app->hz = TONE_MIN_HZ;
            /* Retune live, without dropping the speaker lock. */
            if(app->playing) furi_hal_speaker_start(app->hz, app->volume);
        }

        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
    }

    if(app->playing) furi_hal_speaker_stop();
    if(app->have_speaker) furi_hal_speaker_release();

    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
