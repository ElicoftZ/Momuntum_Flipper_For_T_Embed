/**
 * @file mic_fft.h
 * @brief Shared capture + FFT helpers for the microphone apps.
 *
 * Header-only on purpose: each mic app is a separate FAP with its own link, and
 * buildFap.sh only compiles the .c files inside one app directory. Including
 * this by relative path keeps the maths in one place without a shared library
 * or any build-system changes.
 *
 * A Flipper Zero has no microphone, so none of this exists upstream.
 */
#pragma once

#include <furi.h>
#include <furi_hal_mic.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIC_SAMPLE_RATE 48000u
/* 256 points at 48 kHz = 187.5 Hz per bin, and 128 usable bins -- exactly one
 * per pixel column, which makes every one of these apps trivial to draw. */
#define MIC_FFT_SIZE 256u
#define MIC_BINS     (MIC_FFT_SIZE / 2u)

/** Anything at or above this is inaudible to most adults. */
#define MIC_ULTRASONIC_HZ 17000.0f

#define MIC_BIN_HZ ((float)MIC_SAMPLE_RATE / (float)MIC_FFT_SIZE)

typedef struct {
    int16_t* samples;
    float* re;
    float* im;
    float* tw_cos;
    float* tw_sin;
    float* window;
} MicFft;

static inline bool mic_fft_alloc(MicFft* f) {
    f->samples = malloc(MIC_FFT_SIZE * sizeof(int16_t));
    f->re = malloc(MIC_FFT_SIZE * sizeof(float));
    f->im = malloc(MIC_FFT_SIZE * sizeof(float));
    f->tw_cos = malloc((MIC_FFT_SIZE / 2) * sizeof(float));
    f->tw_sin = malloc((MIC_FFT_SIZE / 2) * sizeof(float));
    f->window = malloc(MIC_FFT_SIZE * sizeof(float));
    if(!f->samples || !f->re || !f->im || !f->tw_cos || !f->tw_sin || !f->window) return false;

    for(size_t i = 0; i < MIC_FFT_SIZE / 2; i++) {
        const float a = -2.0f * (float)M_PI * (float)i / (float)MIC_FFT_SIZE;
        f->tw_cos[i] = cosf(a);
        f->tw_sin[i] = sinf(a);
    }
    /* Hann. Without it a tone sitting between bins smears across the whole
     * spectrum, which matters most here because the interesting signals are
     * narrow tones. Precomputed: it is the same every frame. */
    for(size_t i = 0; i < MIC_FFT_SIZE; i++) {
        f->window[i] =
            0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(MIC_FFT_SIZE - 1)));
    }
    return true;
}

static inline void mic_fft_free(MicFft* f) {
    free(f->window);
    free(f->tw_sin);
    free(f->tw_cos);
    free(f->im);
    free(f->re);
    free(f->samples);
    memset(f, 0, sizeof(*f));
}

/** In-place radix-2 FFT. Small enough that a table-driven butterfly beats
 * pulling in a DSP library the FAP API does not export anyway. */
static inline void mic_fft_run(MicFft* f) {
    for(size_t i = 1, j = 0; i < MIC_FFT_SIZE; i++) {
        size_t bit = MIC_FFT_SIZE >> 1;
        for(; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if(i < j) {
            float t = f->re[i];
            f->re[i] = f->re[j];
            f->re[j] = t;
            t = f->im[i];
            f->im[i] = f->im[j];
            f->im[j] = t;
        }
    }

    for(size_t len = 2; len <= MIC_FFT_SIZE; len <<= 1) {
        const size_t step = MIC_FFT_SIZE / len;
        for(size_t i = 0; i < MIC_FFT_SIZE; i += len) {
            for(size_t k = 0; k < len / 2; k++) {
                const size_t t_idx = k * step;
                const float wr = f->tw_cos[t_idx];
                const float wi = f->tw_sin[t_idx];
                const size_t a = i + k;
                const size_t b = a + len / 2;

                const float xr = f->re[b] * wr - f->im[b] * wi;
                const float xi = f->re[b] * wi + f->im[b] * wr;

                f->re[b] = f->re[a] - xr;
                f->im[b] = f->im[a] - xi;
                f->re[a] += xr;
                f->im[a] += xi;
            }
        }
    }
}

/** Capture one window and fill `mag` (MIC_BINS entries, 0..1, log-scaled).
 *
 * @param      peak_bin  optional, receives the loudest bin index (DC excluded)
 * @return     false when the mic did not deliver a full window
 */
static inline bool mic_fft_capture(MicFft* f, float* mag, size_t* peak_bin) {
    if(furi_hal_mic_read(f->samples, MIC_FFT_SIZE, 200) < MIC_FFT_SIZE) return false;

    for(size_t i = 0; i < MIC_FFT_SIZE; i++) {
        f->re[i] = ((float)f->samples[i] / 32768.0f) * f->window[i];
        f->im[i] = 0.0f;
    }
    mic_fft_run(f);

    float best = 0.0f;
    size_t best_i = 0;
    for(size_t i = 0; i < MIC_BINS; i++) {
        float m = sqrtf(f->re[i] * f->re[i] + f->im[i] * f->im[i]);
        /* Log scale: a beacon 40 dB under a nearby voice still has to show, and
         * a linear magnitude hides exactly that. */
        m = log10f(1.0f + m * 100.0f);
        if(m > 1.0f) m = 1.0f;
        mag[i] = m;
        /* Bin 0-1 is DC and rumble; a PDM mic has plenty and it is never signal. */
        if(i > 1 && m > best) {
            best = m;
            best_i = i;
        }
    }
    if(peak_bin) *peak_bin = best_i;
    return true;
}

/** RMS of the last captured window, as dBFS (negative, 0 = full scale). */
static inline float mic_fft_level_db(const MicFft* f) {
    double sum = 0.0;
    for(size_t i = 0; i < MIC_FFT_SIZE; i++) {
        const double v = (double)f->samples[i] / 32768.0;
        sum += v * v;
    }
    const double rms = sqrt(sum / (double)MIC_FFT_SIZE);
    if(rms < 1e-7) return -120.0f;
    return (float)(20.0 * log10(rms));
}
