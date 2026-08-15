#include <furi.h>
#include <furi_hal.h>

#include <furi_hal_subghz_i.h>

#include <flipper_format/flipper_format_i.h>

#include <subghz/subghz_last_settings.h>

#include "subghz_extended_range.h"

bool subghz_extended_range_load(void) {
    bool enabled = false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);

    /* Momentum's file first, then OFW's, so a card written by either works. */
    if(flipper_format_file_open_existing(file, SUBGHZ_EXTENDED_RANGE_PATH)) {
        flipper_format_read_bool(file, SUBGHZ_EXTENDED_RANGE_KEY, &enabled, 1);
    } else {
        flipper_format_file_close(file);
        if(flipper_format_file_open_existing(file, EXT_PATH("subghz/assets/dangerous_settings"))) {
            flipper_format_read_bool(file, "yes_i_want_to_destroy_my_flipper", &enabled, 1);
        }
    }

    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    return enabled;
}

bool subghz_extended_range_save(bool enabled) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool success = false;

    do {
        if(!flipper_format_file_open_always(file, SUBGHZ_EXTENDED_RANGE_PATH)) break;
        if(!flipper_format_write_header_cstr(file, "Flipper SubGhz Extended Range", 1)) break;
        if(!flipper_format_write_bool(file, SUBGHZ_EXTENDED_RANGE_KEY, &enabled, 1)) break;
        success = true;
    } while(false);

    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);

    /* Applied even if the write failed, so the choice holds for this session. */
    furi_hal_subghz_set_dangerous_frequency(enabled);
    return success;
}

void subghz_dangerous_freq() {
    furi_hal_subghz_set_dangerous_frequency(subghz_extended_range_load());

    // Kept open across the last-settings load below, as before.
    furi_record_open(RECORD_STORAGE);

    // Load external module power amp setting (TODO: move to other place)
    // TODO: Disable this when external module is not CC1101 E07
    SubGhzLastSettings* last_settings = subghz_last_settings_alloc();
    subghz_last_settings_load(last_settings, 0);

    // Set LED and Amp GPIO control state
    furi_hal_subghz_set_ext_leds_and_amp(last_settings->leds_and_amp);

    subghz_last_settings_free(last_settings);

    furi_record_close(RECORD_STORAGE);
}
