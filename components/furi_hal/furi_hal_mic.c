/**
 * @file furi_hal_mic.c
 * @brief PDM microphone capture on I2S_NUM_0.
 *
 * MUST be I2S_NUM_0. The IDF driver rejects PDM on I2S1 outright -- see the
 * check in esp_driver_i2s/i2s_pdm.c: PDM is only supported on I2S0. That is
 * the same controller the speaker uses, but the ESP32-S3 is I2S HW version 2
 * with independent TX and RX clocks, so the speaker keeps the TX channel
 * while capture takes RX. Mic plus speaker is therefore a transceiver, not a
 * choice between the two.
 *
 * PDM RX hands back ordinary 16-bit PCM: the I2S peripheral does the decimation
 * in hardware, so callers get samples, not a bitstream.
 */

#include "furi_hal_mic.h"
#include "boards/board.h"

#include <furi.h>
#include <driver/i2s_pdm.h>
#include <esp_err.h>
#include <esp_log.h>

#define TAG "FuriHalMic"

/* Four DMA buffers of 512 frames: enough that a caller doing an FFT between
 * reads does not drop audio, small enough not to add noticeable latency. */
#define MIC_DMA_DESC_NUM  4
#define MIC_DMA_FRAME_NUM 512

static i2s_chan_handle_t mic_rx_handle = NULL;
static uint32_t mic_sample_rate = 0;

bool furi_hal_mic_start(uint32_t sample_rate) {
    if(mic_rx_handle) {
        FURI_LOG_W(TAG, "already running");
        return false;
    }
    if(sample_rate == 0 || sample_rate > FURI_HAL_MIC_RATE_MAX) {
        FURI_LOG_E(TAG, "bad sample rate %lu", (unsigned long)sample_rate);
        return false;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = MIC_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = MIC_DMA_FRAME_NUM;
    /* Do not auto-clear: a stale tail is better than a stall mid-capture. */
    chan_cfg.auto_clear = false;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &mic_rx_handle);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        mic_rx_handle = NULL;
        return false;
    }

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg =
            I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .clk = (gpio_num_t)BOARD_PIN_MIC_CLK,
                .din = (gpio_num_t)BOARD_PIN_MIC_DATA,
                .invert_flags =
                    {
                        .clk_inv = false,
                    },
            },
    };

    err = i2s_channel_init_pdm_rx_mode(mic_rx_handle, &pdm_cfg);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "init_pdm_rx: %s", esp_err_to_name(err));
        i2s_del_channel(mic_rx_handle);
        mic_rx_handle = NULL;
        return false;
    }

    err = i2s_channel_enable(mic_rx_handle);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "channel_enable: %s", esp_err_to_name(err));
        i2s_del_channel(mic_rx_handle);
        mic_rx_handle = NULL;
        return false;
    }

    mic_sample_rate = sample_rate;
    FURI_LOG_I(TAG, "capture started at %lu Hz", (unsigned long)sample_rate);
    return true;
}

void furi_hal_mic_stop(void) {
    if(!mic_rx_handle) return;

    i2s_channel_disable(mic_rx_handle);
    i2s_del_channel(mic_rx_handle);
    mic_rx_handle = NULL;
    mic_sample_rate = 0;
}

bool furi_hal_mic_is_running(void) {
    return mic_rx_handle != NULL;
}

uint32_t furi_hal_mic_get_sample_rate(void) {
    return mic_sample_rate;
}

size_t furi_hal_mic_read(int16_t* buffer, size_t samples, uint32_t timeout_ms) {
    if(!mic_rx_handle || !buffer || samples == 0) return 0;

    size_t bytes_read = 0;
    const esp_err_t err = i2s_channel_read(
        mic_rx_handle,
        buffer,
        samples * sizeof(int16_t),
        &bytes_read,
        pdMS_TO_TICKS(timeout_ms));

    if(err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        FURI_LOG_E(TAG, "channel_read: %s", esp_err_to_name(err));
        return 0;
    }

    return bytes_read / sizeof(int16_t);
}
