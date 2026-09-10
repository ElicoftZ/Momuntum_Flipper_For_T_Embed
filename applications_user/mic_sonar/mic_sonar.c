/**
 * @file mic_sonar.c
 * @brief Ultrasonic transmit and receive at the same time.
 *
 * The ESP32-S3's I2S controller 0 has independent TX and RX clocks, so the
 * speaker can hold the TX channel while the microphone holds RX. That makes
 * this board an ultrasonic transceiver rather than forcing a choice between
 * playing and listening -- and no Flipper can do either half of it.
 *
 * Emit a tone above hearing, watch its own bin come back. The received level
 * rises when something reflective is close and falls when it is not, which is
 * enough to find a surface, see a door open, or simply prove the speaker
 * reaches a frequency you cannot hear.
 */

#include <furi.h>
#include <furi_hal_mic.h>
#include <furi_hal_speaker.h>

#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>

#include "../mic_common/mic_fft.h"

#define TAG "MicSonar"

/* Tune range: above hearing for most adults, below Nyquist (24 kHz) with room
 * to spare so the bin is not sitting on the edge of the spectrum. */
#define SONAR_HZ_MIN  17000.0f
#define SONAR_HZ_MAX  22000.0f
#define SONAR_HZ_STEP 250.0f

#define SONAR_HISTORY 110

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;

    MicFft fft;
    float mag[MIC_BINS];

    float tone_hz;
    bool emitting;
    bool have_speaker;
    bool running;

    /* Level in the emitted tone's own bin. */
    float rx;
    float rx_min;
    float rx_max;
    float history[SONAR_HISTORY];
    size_t hist_head;
    size_t hist_count;
} MicSonar;

static void sonar_set_tone(MicSonar* app, bool on) {
    if(on && !app->have_speaker) {
        /* Acquire once and hold it: re-acquiring per frame would gate the tone
         * on and off and make the received level meaningless. */
        app->have_speaker = furi_hal_speaker_acquire(1000);
        if(!app->have_speaker) {
            FURI_LOG_E(TAG, "speaker busy");
            return;
        }
    }
    if(!app->have_speaker) return;

    if(on) {
        furi_hal_speaker_start(app->tone_hz, 1.0f);
    } else {
        furi_hal_speaker_stop();
    }
    app->emitting = on;
}

static void sonar_draw_callback(Canvas* canvas, void* ctx) {
    MicSonar* app = ctx;
    if(furi_mutex_acquire(app->mutex, 50) != FuriStatusOk) return;

    if(!app->running) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "Mic failed to start");
        furi_mutex_release(app->mutex);
        return;
    }

    char line[40];

    canvas_set_font(canvas, FontPrimary);
    snprintf(line, sizeof(line), "%.2f kHz", (double)(app->tone_hz / 1000.0f));
    canvas_draw_str(canvas, 2, 10, line);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 74, 10, app->emitting ? "EMITTING" : "silent");

    /* Received level in the tone's own bin. */
    float norm = app->rx;
    if(norm < 0.0f) norm = 0.0f;
    if(norm > 1.0f) norm = 1.0f;
    canvas_draw_str(canvas, 2, 22, "rx");
    canvas_draw_frame(canvas, 16, 15, 110, 8);
    canvas_draw_box(canvas, 17, 16, (int)(norm * 108.0f), 6);

    /* History: the shape over time is what shows a hand or a wall moving. */
    for(size_t i = 0; i < app->hist_count; i++) {
        const size_t idx =
            (app->hist_head + SONAR_HISTORY - app->hist_count + i) % SONAR_HISTORY;
        float n = app->history[idx];
        if(n < 0.0f) n = 0.0f;
        if(n > 1.0f) n = 1.0f;
        const int h = (int)(n * 26.0f);
        if(h > 0) canvas_draw_line(canvas, (int)i + 4, 54, (int)i + 4, 54 - h);
    }
    canvas_draw_line(canvas, 0, 55, 127, 55);

    if(!app->have_speaker && app->emitting) {
        canvas_draw_str(canvas, 2, 64, "speaker busy");
    } else {
        canvas_draw_str(canvas, 2, 64, "wheel: freq   OK: emit");
    }

    furi_mutex_release(app->mutex);
}

static void sonar_input_callback(InputEvent* event, void* ctx) {
    MicSonar* app = ctx;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t mic_sonar_app(void* p) {
    UNUSED(p);

    MicSonar* app = malloc(sizeof(MicSonar));
    memset(app, 0, sizeof(MicSonar));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->tone_hz = 19000.0f;
    app->rx_min = 1.0f;

    if(!mic_fft_alloc(&app->fft)) FURI_LOG_E(TAG, "out of memory");
    app->running = furi_hal_mic_start(MIC_SAMPLE_RATE);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, sonar_draw_callback, app);
    view_port_input_callback_set(app->view_port, sonar_input_callback, app);

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
                sonar_set_tone(app, !app->emitting);
            } else if(event.key == InputKeyUp || event.key == InputKeyDown) {
                app->tone_hz += (event.key == InputKeyUp) ? SONAR_HZ_STEP : -SONAR_HZ_STEP;
                if(app->tone_hz > SONAR_HZ_MAX) app->tone_hz = SONAR_HZ_MIN;
                if(app->tone_hz < SONAR_HZ_MIN) app->tone_hz = SONAR_HZ_MAX;
                /* Retune without dropping the speaker lock. */
                if(app->emitting) furi_hal_speaker_start(app->tone_hz, 1.0f);
                app->hist_count = 0;
                app->hist_head = 0;
            }
            furi_mutex_release(app->mutex);
        }

        if(app->running) {
            size_t peak = 0;
            if(mic_fft_capture(&app->fft, app->mag, &peak)) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);

                /* Read the tone's own bin, plus its neighbours: the emitted
                 * frequency rarely lands exactly on a bin centre. */
                const int centre = (int)(app->tone_hz / MIC_BIN_HZ);
                float level = 0.0f;
                for(int d = -1; d <= 1; d++) {
                    const int b = centre + d;
                    if(b >= 0 && b < (int)MIC_BINS && app->mag[b] > level) level = app->mag[b];
                }
                app->rx = level;
                if(level < app->rx_min) app->rx_min = level;
                if(level > app->rx_max) app->rx_max = level;

                app->history[app->hist_head] = level;
                app->hist_head = (app->hist_head + 1) % SONAR_HISTORY;
                if(app->hist_count < SONAR_HISTORY) app->hist_count++;

                furi_mutex_release(app->mutex);
            }
        } else {
            furi_delay_ms(100);
        }
        view_port_update(app->view_port);
    }

    if(app->emitting) furi_hal_speaker_stop();
    if(app->have_speaker) furi_hal_speaker_release();
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
