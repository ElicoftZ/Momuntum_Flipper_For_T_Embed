#include "bt_settings.h"
#include "bt_settings_filename.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/saved_struct.h>

#define TAG "BtSettings"

#define BT_SETTINGS_PATH    INT_PATH(BT_SETTINGS_FILE_NAME)
#define BT_SETTINGS_VERSION (1)
#define BT_SETTINGS_MAGIC   (0x19)

void bt_settings_load(BtSettings* bt_settings) {
    furi_assert(bt_settings);

    const bool load_success = saved_struct_load(
        BT_SETTINGS_PATH,
        bt_settings,
        sizeof(BtSettings),
        BT_SETTINGS_MAGIC,
        BT_SETTINGS_VERSION);

    if(!load_success) {
        /* Version 0 stored only Bluetooth's enabled flag. Preserve it. */
        bool enabled = true;
        saved_struct_load(BT_SETTINGS_PATH, &enabled, sizeof(enabled), BT_SETTINGS_MAGIC, 0);
        bt_settings->enabled = enabled;
        bt_settings->file_sharing = true;
        bt_settings_save(bt_settings);
    }
}

void bt_settings_save(const BtSettings* bt_settings) {
    furi_assert(bt_settings);

    const bool success = saved_struct_save(
        BT_SETTINGS_PATH,
        bt_settings,
        sizeof(BtSettings),
        BT_SETTINGS_MAGIC,
        BT_SETTINGS_VERSION);

    if(!success) {
        FURI_LOG_E(TAG, "Failed to save settings");
    }
}
