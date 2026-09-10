#include "../spoofing_settings_app.h"
#include "spoofing_settings_scene.h"

#include <furi_hal_version.h>
#include <lib/toolbox/value_index.h>

typedef enum {
    ShellColorRealDefault,
    ShellColorBlack,
    ShellColorWhite,
    ShellColorTransparent,
    ShellColorCount,
} ShellColorIndex;

static const char* const shell_color_text[ShellColorCount] = {
    "Real Default",
    "Black",
    "White",
    "Transparent",
};

static const FuriHalVersionColor shell_color_value[ShellColorCount] = {
    FuriHalVersionColorUnknown,
    FuriHalVersionColorBlack,
    FuriHalVersionColorWhite,
    FuriHalVersionColorTransparent,
};

static void spoofing_settings_scene_shell_color_changed(VariableItem* item) {
    SpoofingSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, shell_color_text[index]);
    /* Live RAM+BLE update; persisted to SD on app exit (see app_free). */
    furi_hal_version_set_hw_color(shell_color_value[index]);
    app->save_color = true;
}

void spoofing_settings_scene_shell_color_on_enter(void* context) {
    SpoofingSettingsApp* app = context;
    VariableItemList* variable_item_list = app->variable_item_list;

    VariableItem* item = variable_item_list_add(
        variable_item_list,
        "Shell Color",
        ShellColorCount,
        spoofing_settings_scene_shell_color_changed,
        app);

    uint8_t current_color = furi_hal_version_get_hw_color();
    uint8_t value_index = 0;
    for(uint8_t i = 0; i < ShellColorCount; i++) {
        if(shell_color_value[i] == current_color) {
            value_index = i;
            break;
        }
    }
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, shell_color_text[value_index]);

    variable_item_list_set_selected_item(variable_item_list, 0);
    view_dispatcher_switch_to_view(
        app->view_dispatcher, SpoofingSettingsAppViewVariableItemList);
}

bool spoofing_settings_scene_shell_color_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void spoofing_settings_scene_shell_color_on_exit(void* context) {
    SpoofingSettingsApp* app = context;
    variable_item_list_reset(app->variable_item_list);
}
