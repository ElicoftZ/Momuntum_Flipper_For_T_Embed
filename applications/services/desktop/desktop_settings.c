#include "desktop_settings.h"
#include "desktop_settings_filename.h"

#include <saved_struct.h>
#include <storage/storage.h>

#define TAG "DesktopSettings"

#define DESKTOP_SETTINGS_VER_14 (14)
#define DESKTOP_SETTINGS_VER_17 (17)
#define DESKTOP_SETTINGS_VER_18 (18)
#define DESKTOP_SETTINGS_VER_19 (19)
#define DESKTOP_SETTINGS_VER    (20)

#define DESKTOP_SETTINGS_PATH  INT_PATH(DESKTOP_SETTINGS_FILE_NAME)
#define DESKTOP_SETTINGS_MAGIC (0x17)

typedef struct {
    uint32_t auto_lock_delay_ms;
    uint8_t displayBatteryPercentage;
    uint8_t dummy_mode;
    uint8_t display_clock;
    FavoriteApp favorite_apps[FavoriteAppNumber];
    FavoriteApp dummy_apps[DummyAppNumber];
} DesktopSettingsV14;

typedef struct {
    uint32_t auto_lock_delay_ms;
    uint8_t usb_inhibit_auto_lock;
    uint8_t displayBatteryPercentage;
    uint8_t dummy_mode;
    uint8_t display_clock;
    FavoriteApp favorite_apps[FavoriteAppNumber];
    FavoriteApp dummy_apps[DummyAppNumber];
} DesktopSettingsV17;

typedef struct {
    uint32_t auto_lock_delay_ms;
    uint8_t usb_inhibit_auto_lock;
    uint8_t displayBatteryPercentage;
    uint8_t dummy_mode;
    uint8_t display_clock;
    FavoriteApp favorite_apps[FavoriteAppNumber];
    FavoriteApp dummy_apps[DummyAppNumber];
    uint8_t hold_ok_action;
} DesktopSettingsV18;

static void desktop_settings_migrate_v17(
    DesktopSettings* settings,
    const DesktopSettingsV17* settings_v17) {
    settings->auto_lock_delay_ms = settings_v17->auto_lock_delay_ms;
    settings->usb_inhibit_auto_lock = settings_v17->usb_inhibit_auto_lock;
    settings->displayBatteryPercentage = settings_v17->displayBatteryPercentage;
    settings->dummy_mode = settings_v17->dummy_mode;
    settings->display_clock = settings_v17->display_clock;
    memcpy(
        settings->favorite_apps,
        settings_v17->favorite_apps,
        sizeof(settings->favorite_apps));
    memcpy(settings->dummy_apps, settings_v17->dummy_apps, sizeof(settings->dummy_apps));
    settings->hold_ok_action = DesktopHoldOkActionFavoriteApp;
    settings->control_center_style = DesktopControlCenterStyleMomentum;
}

static void desktop_settings_migrate_v18(
    DesktopSettings* settings,
    const DesktopSettingsV18* settings_v18) {
    settings->auto_lock_delay_ms = settings_v18->auto_lock_delay_ms;
    settings->usb_inhibit_auto_lock = settings_v18->usb_inhibit_auto_lock;
    settings->displayBatteryPercentage = settings_v18->displayBatteryPercentage;
    settings->dummy_mode = settings_v18->dummy_mode;
    settings->display_clock = settings_v18->display_clock;
    memcpy(
        settings->favorite_apps,
        settings_v18->favorite_apps,
        sizeof(settings->favorite_apps));
    memcpy(settings->dummy_apps, settings_v18->dummy_apps, sizeof(settings->dummy_apps));
    settings->hold_ok_action =
        settings_v18->hold_ok_action < DesktopHoldOkActionCount ?
            settings_v18->hold_ok_action :
            DesktopHoldOkActionFavoriteApp;
    settings->control_center_style = DesktopControlCenterStyleMomentum;
}

// Actual size of DesktopSettings v13
//static_assert(sizeof(DesktopSettingsV13) == 1234);

void desktop_settings_load(DesktopSettings* settings) {
    furi_assert(settings);

    bool success = false;

    do {
        uint8_t version;
        if(!saved_struct_get_metadata(DESKTOP_SETTINGS_PATH, NULL, &version, NULL)) break;

        if(version == DESKTOP_SETTINGS_VER) {
            success = saved_struct_load(
                DESKTOP_SETTINGS_PATH,
                settings,
                sizeof(DesktopSettings),
                DESKTOP_SETTINGS_MAGIC,
                DESKTOP_SETTINGS_VER);

            if(success && settings->hold_ok_action >= DesktopHoldOkActionCount) {
                settings->hold_ok_action = DesktopHoldOkActionFavoriteApp;
            }
            if(success &&
               settings->control_center_style >= DesktopControlCenterStyleCount) {
                settings->control_center_style = DesktopControlCenterStyleMomentum;
            }

        } else if(version == DESKTOP_SETTINGS_VER_19) {
            DesktopSettings* settings_v19 = malloc(sizeof(DesktopSettings));

            success = saved_struct_load(
                DESKTOP_SETTINGS_PATH,
                settings_v19,
                sizeof(DesktopSettings),
                DESKTOP_SETTINGS_MAGIC,
                DESKTOP_SETTINGS_VER_19);

            if(success) {
                memcpy(settings, settings_v19, sizeof(DesktopSettings));
                if(settings->hold_ok_action >= DesktopHoldOkActionCount) {
                    settings->hold_ok_action = DesktopHoldOkActionFavoriteApp;
                }
                /* Momentum is the new default from v20 onward. Users can
                 * switch back to the expanded T-Embed layout in Desktop. */
                settings->control_center_style = DesktopControlCenterStyleMomentum;
            }

            free(settings_v19);

        } else if(version == DESKTOP_SETTINGS_VER_18) {
            DesktopSettingsV18* settings_v18 = malloc(sizeof(DesktopSettingsV18));

            success = saved_struct_load(
                DESKTOP_SETTINGS_PATH,
                settings_v18,
                sizeof(DesktopSettingsV18),
                DESKTOP_SETTINGS_MAGIC,
                DESKTOP_SETTINGS_VER_18);

            if(success) {
                desktop_settings_migrate_v18(settings, settings_v18);
            }

            free(settings_v18);

        } else if(version == DESKTOP_SETTINGS_VER_17) {
            DesktopSettingsV17* settings_v17 = malloc(sizeof(DesktopSettingsV17));

            success = saved_struct_load(
                DESKTOP_SETTINGS_PATH,
                settings_v17,
                sizeof(DesktopSettingsV17),
                DESKTOP_SETTINGS_MAGIC,
                DESKTOP_SETTINGS_VER_17);

            if(success) {
                desktop_settings_migrate_v17(settings, settings_v17);
            }

            free(settings_v17);

        } else if(version == DESKTOP_SETTINGS_VER_14) {
            DesktopSettingsV14* settings_v14 = malloc(sizeof(DesktopSettingsV14));

            success = saved_struct_load(
                DESKTOP_SETTINGS_PATH,
                settings_v14,
                sizeof(DesktopSettingsV14),
                DESKTOP_SETTINGS_MAGIC,
                DESKTOP_SETTINGS_VER_14);

            if(success) {
                settings->auto_lock_delay_ms = settings_v14->auto_lock_delay_ms;
                settings->usb_inhibit_auto_lock = 0;
                settings->displayBatteryPercentage = settings_v14->displayBatteryPercentage;
                settings->dummy_mode = settings_v14->dummy_mode;
                settings->display_clock = settings_v14->display_clock;
                memcpy(
                    settings->favorite_apps,
                    settings_v14->favorite_apps,
                    sizeof(settings->favorite_apps));
                memcpy(
                    settings->dummy_apps, settings_v14->dummy_apps, sizeof(settings->dummy_apps));
                settings->hold_ok_action = DesktopHoldOkActionFavoriteApp;
                settings->control_center_style = DesktopControlCenterStyleMomentum;
            }

            free(settings_v14);
        }

    } while(false);

    if(!success) {
        FURI_LOG_W(TAG, "Failed to load file, using defaults");
        memset(settings, 0, sizeof(DesktopSettings));
        settings->hold_ok_action = DesktopHoldOkActionFavoriteApp;
        settings->control_center_style = DesktopControlCenterStyleMomentum;
        desktop_settings_save(settings);
    }
}

void desktop_settings_save(const DesktopSettings* settings) {
    furi_assert(settings);

    const bool success = saved_struct_save(
        DESKTOP_SETTINGS_PATH,
        settings,
        sizeof(DesktopSettings),
        DESKTOP_SETTINGS_MAGIC,
        DESKTOP_SETTINGS_VER);

    if(!success) {
        FURI_LOG_E(TAG, "Failed to save file");
    }
}
