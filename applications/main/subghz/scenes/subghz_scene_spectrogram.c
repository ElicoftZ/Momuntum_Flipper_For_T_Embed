#include "../subghz_i.h"
#include "../views/subghz_spectrogram.h"

static void subghz_scene_spectrogram_callback(SubGhzCustomEvent event, void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

void subghz_scene_spectrogram_on_enter(void* context) {
    SubGhz* subghz = context;
    subghz_spectrogram_set_callback(
        subghz->subghz_spectrogram, subghz_scene_spectrogram_callback, subghz);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdSpectrogram);
}

bool subghz_scene_spectrogram_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == SubGhzCustomEventViewSpectrogramBack) {
        scene_manager_previous_scene(subghz->scene_manager);
        return true;
    }
    return false;
}

void subghz_scene_spectrogram_on_exit(void* context) {
    UNUSED(context);
}

