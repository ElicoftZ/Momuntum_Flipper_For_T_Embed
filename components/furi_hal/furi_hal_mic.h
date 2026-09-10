/**
 * @file furi_hal_mic.h
 * @brief PDM microphone capture.
 *
 * The T-Embed carries a PDM MEMS microphone on IO42/IO39. A Flipper Zero has no
 * microphone at all, so nothing upstream uses this -- it exists for apps that
 * want audio in (spectrum/ultrasonic work, DTMF, level metering).
 *
 * Uses I2S_NUM_0 -- the IDF driver only supports PDM on that controller. The
 * speaker shares it via the TX channel; RX is independent on this chip, so
 * capture and playback still run together.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Highest rate the PDM front end is configured to accept. 48 kHz puts Nyquist
 * at 24 kHz, which covers the 18-20 kHz band ultrasonic beacons use. */
#define FURI_HAL_MIC_RATE_MAX 48000U

/** Start capture.
 *
 * @param      sample_rate  Hz, e.g. 16000 for speech, 48000 for ultrasonic.
 * @return     true on success, false if already running or the driver failed.
 */
bool furi_hal_mic_start(uint32_t sample_rate);

/** Stop capture and release the I2S channel. Safe to call when not running. */
void furi_hal_mic_stop(void);

/** @return true while capture is active. */
bool furi_hal_mic_is_running(void);

/** Read mono 16-bit samples.
 *
 * Blocks until the buffer is filled or the timeout expires, so a caller that
 * wants a fixed FFT window can just ask for it.
 *
 * @param      buffer      destination, at least `samples` int16 values
 * @param      samples     how many samples to read
 * @param      timeout_ms  give up after this long
 * @return     samples actually read (0 on timeout or when not running)
 */
size_t furi_hal_mic_read(int16_t* buffer, size_t samples, uint32_t timeout_ms);

/** @return the rate capture was started at, or 0 when stopped. */
uint32_t furi_hal_mic_get_sample_rate(void);

#ifdef __cplusplus
}
#endif
