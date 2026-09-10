#pragma once

#include <gui/gui.h>
#include <gui/view.h>

#include "../helpers/subghz_custom_event.h"

typedef struct SubGhzSpectrogram SubGhzSpectrogram;
typedef void (*SubGhzSpectrogramCallback)(SubGhzCustomEvent event, void* context);

SubGhzSpectrogram* subghz_spectrogram_alloc(Gui* gui);
void subghz_spectrogram_free(SubGhzSpectrogram* instance);
View* subghz_spectrogram_get_view(SubGhzSpectrogram* instance);

void subghz_spectrogram_set_callback(
    SubGhzSpectrogram* instance,
    SubGhzSpectrogramCallback callback,
    void* context);

