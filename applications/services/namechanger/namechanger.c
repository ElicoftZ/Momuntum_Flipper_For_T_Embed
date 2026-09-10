#include "namechanger.h"
#include <wifi/wlan_hal.h>
#include <furi_hal.h>
#include <furi_hal_version.h>
#include <cli/cli_vcp.h>
#include <bt/bt_service/bt.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#define TAG "NameChanger"

static bool namechanger_init() {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    // Kostil + velosiped = top ficha
    uint8_t timeout = 0;
    while(timeout < 11) {
        if(storage_sd_status(storage) == FSE_OK) break;
        furi_delay_ms(250);
        timeout++;
        /*if(timeout == 10) {
            // Failed to init namechanger, SD card not ready
            furi_record_close(RECORD_STORAGE);
            return false;
        }*/
    }

    FuriString* str = furi_string_alloc();
    FlipperFormat* file = flipper_format_file_alloc(storage);

    bool res = false;

    /* Every failure below used to be a bare `break`, so a rejected name looked
     * exactly like no name at all: the device silently kept its MAC-derived
     * default and nothing said why. Say which check failed -- the rules here
     * (2-8 chars, alphanumeric only) are stricter than the text field that
     * writes the file, so a perfectly reasonable name can be refused. */
    do {
        uint32_t version;
        if(!flipper_format_file_open_existing(file, NAMECHANGER_PATH)) {
            FURI_LOG_W(TAG, "no name file at %s", NAMECHANGER_PATH);
            break;
        }
        if(!flipper_format_read_header(file, str, &version)) {
            FURI_LOG_W(TAG, "name file has no readable header");
            break;
        }
        if(furi_string_cmp_str(str, NAMECHANGER_HEADER)) {
            FURI_LOG_W(TAG, "wrong header \"%s\"", furi_string_get_cstr(str));
            break;
        }
        if(version != NAMECHANGER_VERSION) {
            FURI_LOG_W(TAG, "wrong version %lu", (unsigned long)version);
            break;
        }

        if(!flipper_format_read_string(file, "Name", str)) {
            FURI_LOG_W(TAG, "name file has no Name key");
            break;
        }
        // Check for size
        size_t temp_string_size = furi_string_size(str);
        if(temp_string_size > (size_t)8) {
            FURI_LOG_W(
                TAG,
                "name \"%s\" is %u chars, max 8",
                furi_string_get_cstr(str),
                (unsigned)temp_string_size);
            break;
        }
        if(temp_string_size < (size_t)2) {
            FURI_LOG_W(TAG, "name is shorter than 2 chars");
            break;
        }

        // Check for forbidden characters
        const char* name_ptr = furi_string_get_cstr(str);
        bool chars_check_failed = false;

        for(; *name_ptr; ++name_ptr) {
            const char c = *name_ptr;
            if((c < '0' || c > '9') && (c < 'A' || c > 'Z') && (c < 'a' || c > 'z')) {
                chars_check_failed = true;
                break;
            }
        }

        if(chars_check_failed) {
            FURI_LOG_W(
                TAG,
                "name \"%s\" has a character outside 0-9 A-Z a-z (spaces are not allowed)",
                furi_string_get_cstr(str));
            break;
        }

        // If all checks was good we can set the name.
        // NB: call furi_hal_version_set_name() directly with the string — it
        // copies the name into all live fields AND updates the Version custom
        // name via refresh_names(). Going through version_set_custom_name(NULL,
        // …) is a no-op (it early-returns on a NULL Version*), which used to
        // leave the name at the eFuse-derived default.
        furi_hal_version_set_name(furi_string_get_cstr(str));

        /* Optional shell color (added when NVS persistence moved to SD). Older
         * files without the field simply keep the default color. */
        uint32_t color;
        if(flipper_format_read_uint32(file, "Color", &color, 1)) {
            furi_hal_version_set_hw_color((FuriHalVersionColor)color);
        }

        res = true;
    } while(false);

    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    furi_string_free(str);

    return res;
}

int32_t namechanger_on_system_start(void* p) {
    UNUSED(p);
    if(furi_hal_rtc_get_boot_mode() != FuriHalRtcBootModeNormal) {
        return 0;
    }

    // Wait for all required services to start and create their records
    uint8_t timeout = 0;
    while(!furi_record_exists(RECORD_CLI_VCP) || !furi_record_exists(RECORD_BT) ||
          !furi_record_exists(RECORD_STORAGE)) {
        timeout++;
        if(timeout > 250) {
            return 0;
        }
        furi_delay_ms(5);
    }

    // Hehe bad code now here, bad bad bad, very bad, bad example, dont take it, make it better

    if(namechanger_init()) {
        CliVcp* cli = furi_record_open(RECORD_CLI_VCP);
        cli_vcp_disable(cli);
        furi_delay_ms(2); // why i added delays here
        cli_vcp_enable(cli);
        furi_record_close(RECORD_CLI_VCP);

        furi_delay_ms(3);
        Bt* bt = furi_record_open(RECORD_BT);
        /* Update GAP and advertising data in place. Cycling the whole NimBLE
         * host here raced its old host task teardown, made the restart time out,
         * and removed the BLE status icon on every boot with a custom name. */
        if(bt_is_enabled(bt) && !wlan_hal_is_user_enabled() && !wlan_hal_is_started()) {
            if(!bt_refresh_device_name(bt)) {
                FURI_LOG_W(TAG, "Could not refresh the active BLE profile");
            }
        } else {
            FURI_LOG_I(TAG, "BLE profile refresh deferred while BLE is off or WiFi owns radio");
        }
        furi_record_close(RECORD_BT);
        bt = NULL;
        furi_delay_ms(3);
    }

    return 0;
}
