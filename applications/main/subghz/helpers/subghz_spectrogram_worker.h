#pragma once

#include <furi.h>

#define SUBGHZ_SPECTROGRAM_BINS 128U

typedef struct SubGhzSpectrogramWorker SubGhzSpectrogramWorker;

typedef void (*SubGhzSpectrogramWorkerCallback)(
    void* context,
    const int8_t* rssi_bins,
    size_t bin_count,
    uint32_t range_start,
    uint32_t range_stop,
    uint32_t peak_frequency,
    int8_t peak_rssi);

SubGhzSpectrogramWorker* subghz_spectrogram_worker_alloc(void);
void subghz_spectrogram_worker_free(SubGhzSpectrogramWorker* instance);

void subghz_spectrogram_worker_set_callback(
    SubGhzSpectrogramWorker* instance,
    SubGhzSpectrogramWorkerCallback callback,
    void* context);

void subghz_spectrogram_worker_set_range(
    SubGhzSpectrogramWorker* instance,
    uint32_t start,
    uint32_t stop,
    bool narrow_bandwidth);

void subghz_spectrogram_worker_set_paused(SubGhzSpectrogramWorker* instance, bool paused);
void subghz_spectrogram_worker_start(SubGhzSpectrogramWorker* instance);
void subghz_spectrogram_worker_stop(SubGhzSpectrogramWorker* instance);
bool subghz_spectrogram_worker_is_running(SubGhzSpectrogramWorker* instance);

