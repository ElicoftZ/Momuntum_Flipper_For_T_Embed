#include "../bt_settings_app.h"
#include <furi_hal_bt.h>

enum BtSetting {
    BtSettingOff,
    BtSettingOn,
    BtSettingNum,
};

enum BtSettingIndex {
    BtSettingIndexSwitchBt,
    BtSettingIndexFileSharing,
    BtSettingIndexForgetDev,
};

const char* const bt_settings_text[BtSettingNum] = {
    "OFF",
    "ON",
};

static void bt_settings_scene_start_var_list_change_callback(VariableItem* item) {
    BtSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, bt_settings_text[index]);
    uint32_t row = variable_item_list_get_selected_item_index(app->var_item_list);
    view_dispatcher_send_custom_event(app->view_dispatcher, row * BtSettingNum + index);
}

static void bt_settings_scene_start_var_list_enter_callback(void* context, uint32_t index) {
    furi_assert(context);
    BtSettingsApp* app = context;
    if(index == BtSettingIndexForgetDev) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, BtSettingsCustomEventForgetDevices);
    }
}

void bt_settings_scene_start_on_enter(void* context) {
    BtSettingsApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;

    if(furi_hal_bt_is_gatt_gap_supported()) {
        item = variable_item_list_add(
            var_item_list,
            "Bluetooth",
            BtSettingNum,
            bt_settings_scene_start_var_list_change_callback,
            app);
        if(app->settings.enabled) {
            variable_item_set_current_value_index(item, BtSettingOn);
            variable_item_set_current_value_text(item, bt_settings_text[BtSettingOn]);
        } else {
            variable_item_set_current_value_index(item, BtSettingOff);
            variable_item_set_current_value_text(item, bt_settings_text[BtSettingOff]);
        }
        item = variable_item_list_add(
            var_item_list,
            "File Sharing",
            BtSettingNum,
            bt_settings_scene_start_var_list_change_callback,
            app);
        uint8_t sharing = app->settings.file_sharing ? BtSettingOn : BtSettingOff;
        variable_item_set_current_value_index(item, sharing);
        variable_item_set_current_value_text(item, bt_settings_text[sharing]);
        variable_item_list_add(var_item_list, "Unpair All Devices", 1, NULL, NULL);
        variable_item_list_set_enter_callback(
            var_item_list, bt_settings_scene_start_var_list_enter_callback, app);
    } else {
        item = variable_item_list_add(var_item_list, "Bluetooth", 1, NULL, NULL);
        variable_item_set_current_value_text(item, "Broken");
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, BtSettingsAppViewVarItemList);
}

bool bt_settings_scene_start_on_event(void* context, SceneManagerEvent event) {
    BtSettingsApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == BtSettingOn) {
            app->settings.enabled = true;
            consumed = true;
        } else if(event.event == BtSettingOff) {
            app->settings.enabled = false;
            consumed = true;
        } else if(event.event == BtSettingIndexFileSharing * BtSettingNum + BtSettingOn) {
            app->settings.file_sharing = true;
            consumed = true;
        } else if(event.event == BtSettingIndexFileSharing * BtSettingNum + BtSettingOff) {
            app->settings.file_sharing = false;
            consumed = true;
        } else if(event.event == BtSettingsCustomEventForgetDevices) {
            scene_manager_next_scene(app->scene_manager, BtSettingsAppSceneForgetDevConfirm);
            consumed = true;
        }
    }

    return consumed;
}

void bt_settings_scene_start_on_exit(void* context) {
    BtSettingsApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
