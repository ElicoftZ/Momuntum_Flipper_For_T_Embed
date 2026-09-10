#pragma once

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/popup.h>

#include <furi_hal_version.h>

typedef enum {
    SpoofingSettingsAppViewMenu,
    SpoofingSettingsAppViewTextInput,
    SpoofingSettingsAppViewVariableItemList,
    SpoofingSettingsAppViewPopup,
} SpoofingSettingsAppView;

typedef struct {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextInput* text_input;
    VariableItemList* variable_item_list;
    Popup* popup;

    bool save_name;
    bool save_color;
    char device_name[FURI_HAL_VERSION_ARRAY_NAME_LENGTH];
} SpoofingSettingsApp;
