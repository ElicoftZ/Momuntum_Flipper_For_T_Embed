/**
 * @file ultrasonic.c
 * @brief Audio + ultrasonic spectrum analyser for the T-Embed's PDM mic.
 *
 * A Flipper Zero has no microphone, so nothing like this exists upstream --
 * every "spectrum analyser" for Flipper is Sub-GHz. This one is acoustic.
 *
 * Sampling at 48 kHz puts Nyquist at 24 kHz, which covers the 18-20 kHz band
 * that retail/ad-tech ultrasonic beacons use to pair a phone to a display, and
 * the 20-25 kHz region pest and dog repellers sit in. Both are inaudible and
 * otherwise invisible without lab kit.
 */

#include <furi.h>
#include <furi_hal_mic.h>

#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAG "Ultrasonic"

#define US_SAMPLE_RATE 48000u
/* 256-point FFT: 187.5 Hz per bin at 48 kHz, and 128 usable bins -- one per
 * pixel column of the display, which makes the drawing trivial. */
#define US_FFT_SIZE 256u
#define US_BINS     (US_FFT_SIZE / 2u)

/* Everything at or above this is inaudible to most adults, and is what makes
 * this app worth having. */
#define US_ULTRASONIC_HZ 17000.0f

#define US_GRAPH_TOP 18
#define US_GRAPH_BOT 55

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;

    int16_t* samples;
    float* re;
    float* im;
    float* twiddle_cos;
    float* twiddle_sin;

    /* Magnitude per bin, 0..1 after normalisation. */
    float mag[US_BINS];
    float peak_hold[US_BINS];

    float peak_hz;
    float peak_mag;
    bool hold;
    bool running;
} Ultrasonic;

/* ------------------------------------------------------------------ */
/* FFT                                                                 */
/* ------------------------------------------------------------------ */

static void us_fft_init(Ultrasonic* app) {
    for(size_t i = 0; i < US_FFT_SIZE / 2; i++) {
        const float a = -2.0f * (float)M_PI * (float)i / (float)US_FFT_SIZE;
        app->twiddle_cos[i] = cosf(a);
        app->twiddle_sin[i] = sinf(a);
    }
}

/** In-place radix-2 FFT. Small enough that a table-driven butterfly beats
 * pulling in a DSP library the FAP API does not export anyway. */
static void us_fft(Ultrasonic* app) {
    /* Bit-reversal permutation. */
    for(size_t i = 1, j = 0; i < US_FFT_SIZE; i++) {
        size_t bit = US_FFT_SIZE >> 1;
        for(; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if(i < j) {
            float t = app->re[i];
            app->re[i] = app->re[j];
            app->re[j] = t;
            t = app->im[i];
            app->im[i] = app->im[j];
            app->im[j] = t;
        }
    }

    for(size_t len = 2; len <= US_FFT_SIZE; len <<= 1) {
        const size_t step = US_FFT_SIZE / len;
        for(size_t i = 0; i < US_FFT_SIZE; i += len) {
            for(size_t k = 0; k < len / 2; k++) {
                const size_t t_idx = k * step;
                const float wr = app->twiddle_cos[t_idx];
                const float wi = app->twiddle_sin[t_idx];

                const size_t a = i + k;
                const size_t b = i + k + len / 2;

                const float xr = app->re[b] * wr - app->im[b] * wi;
                const float xi = app->re[b] * wi + app->im[b] * wr;

                app->re[b] = app->re[a] - xr;
                app->im[b] = app->im[a] - xi;
                app->re[a] += xr;
                app->im[a] += xi;
            }
        }
    }
}

/** Capture one window and turn it into normalised magnitudes. */
static void us_analyse(Ultrasonic* app) {
    const size_t got = furi_hal_mic_read(app->samples, US_FFT_SIZE, 200);
    if(got < US_FFT_SIZE) return;

    /* Hann window: without it a tone between bins smears across the whole
     * spectrum, which matters here because the interesting signals are narrow. */
    for(size_t i = 0; i < US_FFT_SIZE; i++) {
        const float w =
            0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(US_FFT_SIZE - 1)));
        app->re[i] = ((float)app->samples[i] / 32768.0f) * w;
        app->im[i] = 0.0f;
    }

    us_fft(app);

    const float bin_hz = (float)US_SAMPLE_RATE / (float)US_FFT_SIZE;
    float peak = 0.0f;
    float peak_hz = 0.0f;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    for(size_t i = 0; i < US_BINS; i++) {
        float m = sqrtf(app->re[i] * app->re[i] + app->im[i] * app->im[i]);
        /* Log scale: a beacon 40 dB below a nearby voice still has to be
         * visible, and linear magnitudes hide exactly that. */
        m = log10f(1.0f + m * 100.0f);
        if(m > 1.0f) m = 1.0f;

        app->mag[i] = m;
        if(m > app->peak_hold[i]) app->peak_hold[i] = m;
        /* Bin 0 is DC; a PDM mic has plenty and it is never the signal. */
        if(i > 1 && m > peak) {
            peak = m;
            peak_hz = (float)i * bin_hz;
        }
    }
    app->peak_mag = peak;
    app->peak_hz = peak_hz;
    furi_mutex_release(app->mutex);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void us_draw_callback(Canvas* canvas, void* ctx) {
    Ultrasonic* app = ctx;
    if(furi_mutex_acquire(app->mutex, 50) != FuriStatusOk) return;

    char line[40];

    canvas_set_font(canvas, FontSecondary);

    if(!app->running) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "Mic failed to start");
        furi_mutex_release(app->mutex);
        return;
    }

    /* Header: peak frequency, and whether it is above hearing. */
    const bool ultra = app->peak_hz >= US_ULTRASONIC_HZ && app->peak_mag > 0.15f;
    if(app->peak_mag > 0.08f) {
        snprintf(
            line,
            sizeof(line),
            "%s%.1f kHz",
            ultra ? "ULTRASONIC " : "peak ",
            (double)(app->peak_hz / 1000.0f));
    } else {
        snprintf(line, sizeof(line), "quiet");
    }
    canvas_draw_str(canvas, 2, 9, line);

    if(app->hold) canvas_draw_str(canvas, 100, 9, "HOLD");

    /* Spectrum: one column per bin, 128 bins across 128 px. */
    const uint8_t h = US_GRAPH_BOT - US_GRAPH_TOP;
    for(size_t i = 0; i < US_BINS; i++) {
        const uint8_t bar = (uint8_t)(app->mag[i] * (float)h);
        if(bar > 0) {
            canvas_draw_line(canvas, (int)i, US_GRAPH_BOT, (int)i, US_GRAPH_BOT - bar);
        }
        if(app->hold) {
            const uint8_t ph = (uint8_t)(app->peak_hold[i] * (float)h);
            if(ph > 0) canvas_draw_dot(canvas, (int)i, US_GRAPH_BOT - ph);
        }
    }

    /* Mark where hearing ends, so an ultrasonic signal is obvious by position
     * and not only by the header text. */
    const float bin_hz = (float)US_SAMPLE_RATE / (float)US_FFT_SIZE;
    const int ultra_x = (int)(US_ULTRASONIC_HZ / bin_hz);
    for(int y = US_GRAPH_TOP; y < US_GRAPH_BOT; y += 3) {
        canvas_draw_dot(canvas, ultra_x, y);
    }

    canvas_draw_line(canvas, 0, US_GRAPH_BOT, 127, US_GRAPH_BOT);
    canvas_draw_str(canvas, 2, 63, "0");
    canvas_draw_str(canvas, ultra_x - 6, 63, "17k");
    canvas_draw_str(canvas, 108, 63, "24k");

    furi_mutex_release(app->mutex);
}

static void us_input_callback(InputEvent* event, void* ctx) {
    Ultrasonic* app = ctx;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int32_t ultrasonic_app(void* p) {
    UNUSED(p);

    Ultrasonic* app = malloc(sizeof(Ultrasonic));
    memset(app, 0, sizeof(Ultrasonic));

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->samples = malloc(US_FFT_SIZE * sizeof(int16_t));
    app->re = malloc(US_FFT_SIZE * sizeof(float));
    app->im = malloc(US_FFT_SIZE * sizeof(float));
    app->twiddle_cos = malloc((US_FFT_SIZE / 2) * sizeof(float));
    app->twiddle_sin = malloc((US_FFT_SIZE / 2) * sizeof(float));

    us_fft_init(app);

    app->running = furi_hal_mic_start(US_SAMPLE_RATE);
    if(!app->running) {
        FURI_LOG_E(TAG, "furi_hal_mic_start failed");
    }

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, us_draw_callback, app);
    view_port_input_callback_set(app->view_port, us_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    bool running = true;
    while(running) {
        InputEvent event;
        /* Poll input without blocking: the capture loop has to keep running or
         * the spectrum freezes. */
        while(furi_message_queue_get(app->queue, &event, 0) == FuriStatusOk) {
            if(event.type != InputTypeShort) continue;

            if(event.key == InputKeyBack) {
                running = false;
            } else if(event.key == InputKeyOk) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->hold = !app->hold;
                if(!app->hold) memset(app->peak_hold, 0, sizeof(app->peak_hold));
                furi_mutex_release(app->mutex);
            } else if(event.key == InputKeyUp || event.key == InputKeyDown) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                memset(app->peak_hold, 0, sizeof(app->peak_hold));
                furi_mutex_release(app->mutex);
            }
        }

        if(app->running) {
            us_analyse(app);
        } else {
            furi_delay_ms(100);
        }
        view_port_update(app->view_port);
    }

    furi_hal_mic_stop();

    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    free(app->twiddle_sin);
    free(app->twiddle_cos);
    free(app->im);
    free(app->re);
    free(app->samples);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
