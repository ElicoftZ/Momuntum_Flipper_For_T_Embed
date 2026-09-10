#include "subghz_spectrogram_worker.h"

#include <furi_hal_subghz.h>
#include <furi_hal_spi.h>
#include <lib/drivers/cc1101.h>

#define TAG "SubGhzSpectrogramWorker"

static const uint8_t subghz_spectrogram_bw_narrow[][2] = {
    {CC1101_MDMCFG4, 0b11110111}, /* 58.0 kHz RX filter */
    {0, 0},
};

static const uint8_t subghz_spectrogram_bw_wide[][2] = {
    {CC1101_MDMCFG4, 0b00010111}, /* 650 kHz RX filter */
    {0, 0},
};

struct SubGhzSpectrogramWorker {
    FuriThread* thread;
    FuriMutex* config_mutex;
    volatile bool running;
    volatile bool paused;
    volatile uint32_t config_generation;
    uint32_t range_start;
    uint32_t range_stop;
    bool narrow_bandwidth;
    SubGhzSpectrogramWorkerCallback callback;
    void* callback_context;
};

static void subghz_spectrogram_set_path(uint32_t frequency) {
    if(frequency >= 281000000U && frequency <= 361000000U) {
        furi_hal_subghz_set_path(FuriHalSubGhzPath315);
    } else if(frequency >= 378000000U && frequency <= 481000000U) {
        furi_hal_subghz_set_path(FuriHalSubGhzPath433);
    } else if(frequency >= 749000000U && frequency <= 962000000U) {
        furi_hal_subghz_set_path(FuriHalSubGhzPath868);
    } else {
        furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);
    }
}

static void subghz_spectrogram_load_registers(const uint8_t registers[][2]) {
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    for(size_t i = 0; registers[i][0]; i++) {
        cc1101_write_reg(
            &furi_hal_spi_bus_handle_subghz, registers[i][0], registers[i][1]);
    }
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
}

static void subghz_spectrogram_configure_radio(void) {
    furi_hal_subghz_reset();

    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_flush_rx(&furi_hal_spi_bus_handle_subghz);
    cc1101_flush_tx(&furi_hal_spi_bus_handle_subghz);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_IOCFG0, CC1101IocfgHW);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_MDMCFG3, 0b01111111);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL2, 0b00000111);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL1, 0b00001000);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL0, 0b00110000);
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
}

static int8_t subghz_spectrogram_clamp_rssi(float rssi) {
    if(rssi < -127.0f) return -127;
    if(rssi > 0.0f) return 0;
    return (int8_t)rssi;
}

static int32_t subghz_spectrogram_worker_thread(void* context) {
    SubGhzSpectrogramWorker* instance = context;
    int8_t rssi_bins[SUBGHZ_SPECTROGRAM_BINS];

    subghz_spectrogram_configure_radio();
    furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);

    bool configured_narrow = false;
    bool bandwidth_configured = false;

    while(instance->running) {
        if(instance->paused) {
            furi_hal_subghz_idle();
            furi_delay_ms(40);
            continue;
        }

        uint32_t range_start;
        uint32_t range_stop;
        bool narrow_bandwidth;
        uint32_t generation;
        furi_check(
            furi_mutex_acquire(instance->config_mutex, FuriWaitForever) == FuriStatusOk);
        range_start = instance->range_start;
        range_stop = instance->range_stop;
        narrow_bandwidth = instance->narrow_bandwidth;
        generation = instance->config_generation;
        furi_mutex_release(instance->config_mutex);

        if(!bandwidth_configured || configured_narrow != narrow_bandwidth) {
            furi_hal_subghz_idle();
            subghz_spectrogram_load_registers(
                narrow_bandwidth ? subghz_spectrogram_bw_narrow :
                                   subghz_spectrogram_bw_wide);
            configured_narrow = narrow_bandwidth;
            bandwidth_configured = true;
        }

        subghz_spectrogram_set_path(range_start);
        int8_t peak_rssi = -127;
        uint32_t peak_frequency = range_start;
        bool row_complete = true;

        for(size_t i = 0; i < SUBGHZ_SPECTROGRAM_BINS; i++) {
            if(!instance->running || instance->paused ||
               generation != instance->config_generation) {
                row_complete = false;
                break;
            }

            uint32_t frequency = range_start + (uint32_t)(
                ((uint64_t)(range_stop - range_start) * i) /
                (SUBGHZ_SPECTROGRAM_BINS - 1U));

            if(!furi_hal_subghz_is_frequency_valid(frequency)) {
                rssi_bins[i] = -127;
                continue;
            }

            furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
            cc1101_switch_to_idle(&furi_hal_spi_bus_handle_subghz);
            uint32_t actual_frequency =
                cc1101_set_frequency(&furi_hal_spi_bus_handle_subghz, frequency);
            cc1101_calibrate(&furi_hal_spi_bus_handle_subghz);
            bool ready = cc1101_wait_status_state(
                &furi_hal_spi_bus_handle_subghz, CC1101StateIDLE, 10000);
            if(ready) cc1101_switch_to_rx(&furi_hal_spi_bus_handle_subghz);
            furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);

            if(!ready) {
                rssi_bins[i] = -127;
                continue;
            }

            furi_delay_ms(2);
            int8_t rssi = subghz_spectrogram_clamp_rssi(furi_hal_subghz_get_rssi());
            rssi_bins[i] = rssi;
            if(rssi > peak_rssi) {
                peak_rssi = rssi;
                peak_frequency = actual_frequency;
            }
        }

        if(row_complete && instance->callback) {
            instance->callback(
                instance->callback_context,
                rssi_bins,
                SUBGHZ_SPECTROGRAM_BINS,
                range_start,
                range_stop,
                peak_frequency,
                peak_rssi);
        }
    }

    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
    furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);
    return 0;
}

SubGhzSpectrogramWorker* subghz_spectrogram_worker_alloc(void) {
    SubGhzSpectrogramWorker* instance = calloc(1, sizeof(SubGhzSpectrogramWorker));
    instance->config_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->thread = furi_thread_alloc_ex(
        "SubGhzSpectrum", 4096, subghz_spectrogram_worker_thread, instance);
    return instance;
}

void subghz_spectrogram_worker_free(SubGhzSpectrogramWorker* instance) {
    furi_assert(instance);
    furi_assert(!instance->running);
    furi_thread_free(instance->thread);
    furi_mutex_free(instance->config_mutex);
    free(instance);
}

void subghz_spectrogram_worker_set_callback(
    SubGhzSpectrogramWorker* instance,
    SubGhzSpectrogramWorkerCallback callback,
    void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->callback_context = context;
}

void subghz_spectrogram_worker_set_range(
    SubGhzSpectrogramWorker* instance,
    uint32_t start,
    uint32_t stop,
    bool narrow_bandwidth) {
    furi_assert(instance);
    furi_assert(start < stop);
    furi_check(furi_mutex_acquire(instance->config_mutex, FuriWaitForever) == FuriStatusOk);
    instance->range_start = start;
    instance->range_stop = stop;
    instance->narrow_bandwidth = narrow_bandwidth;
    instance->config_generation++;
    furi_mutex_release(instance->config_mutex);
}

void subghz_spectrogram_worker_set_paused(SubGhzSpectrogramWorker* instance, bool paused) {
    furi_assert(instance);
    instance->paused = paused;
}

void subghz_spectrogram_worker_start(SubGhzSpectrogramWorker* instance) {
    furi_assert(instance);
    furi_assert(!instance->running);
    instance->paused = false;
    instance->running = true;
    furi_thread_start(instance->thread);
}

void subghz_spectrogram_worker_stop(SubGhzSpectrogramWorker* instance) {
    furi_assert(instance);
    if(!instance->running) return;
    instance->running = false;
    furi_thread_join(instance->thread);
}

bool subghz_spectrogram_worker_is_running(SubGhzSpectrogramWorker* instance) {
    furi_assert(instance);
    return instance->running;
}

