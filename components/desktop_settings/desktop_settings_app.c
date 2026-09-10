#include <furi.h>

#include <desktop/desktop.h>
#include <gui/gui.h>
#include <gui/modules/variable_item_list.h>
#include <gui/view_dispatcher.h>

typedef struct {
    Gui* gui;
    Desktop* desktop;
    ViewDispatcher* view_dispatcher;
    VariableItemList* variable_item_list;
    DesktopSettings settings;
} ShortcutSettingsApp;

static const char* const hold_ok_action_text[DesktopHoldOkActionCount] = {
    "Favorite App",
    "Control Center",
};

static const char* const control_center_style_text[DesktopControlCenterStyleCount] = {
    "T-Embed",
    "Momentum",
};

static void shortcut_settings_control_center_style_changed(VariableItem* item) {
    ShortcutSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    if(index >= DesktopControlCenterStyleCount) index = DesktopControlCenterStyleMomentum;
    variable_item_set_current_value_text(item, control_center_style_text[index]);
    app->settings.control_center_style = index;
}

static void shortcut_settings_hold_ok_changed(VariableItem* item) {
    ShortcutSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    if(index >= DesktopHoldOkActionCount) index = DesktopHoldOkActionFavoriteApp;
    variable_item_set_current_value_text(item, hold_ok_action_text[index]);
    app->settings.hold_ok_action = index;
}

static bool shortcut_settings_back_callback(void* context) {
    ShortcutSettingsApp* app = context;
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

int32_t desktop_settings_app(void* p) {
    UNUSED(p);

    ShortcutSettingsApp* app = malloc(sizeof(ShortcutSettingsApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->desktop = furi_record_open(RECORD_DESKTOP);
    desktop_api_get_settings(app->desktop, &app->settings);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, shortcut_settings_back_callback);
    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->variable_item_list = variable_item_list_alloc();
    variable_item_list_set_header(app->variable_item_list, "Desktop Settings");

    VariableItem* control_center_style_item = variable_item_list_add(
        app->variable_item_list,
        "Control Center",
        DesktopControlCenterStyleCount,
        shortcut_settings_control_center_style_changed,
        app);

    uint8_t control_center_style_index = app->settings.control_center_style;
    if(control_center_style_index >= DesktopControlCenterStyleCount) {
        control_center_style_index = DesktopControlCenterStyleMomentum;
    }
    variable_item_set_current_value_index(
        control_center_style_item, control_center_style_index);
    variable_item_set_current_value_text(
        control_center_style_item, control_center_style_text[control_center_style_index]);

    VariableItem* hold_ok_item = variable_item_list_add(
        app->variable_item_list,
        "Hold OK",
        DesktopHoldOkActionCount,
        shortcut_settings_hold_ok_changed,
        app);

    uint8_t value_index = app->settings.hold_ok_action;
    if(value_index >= DesktopHoldOkActionCount) value_index = DesktopHoldOkActionFavoriteApp;
    variable_item_set_current_value_index(hold_ok_item, value_index);
    variable_item_set_current_value_text(hold_ok_item, hold_ok_action_text[value_index]);

    view_dispatcher_add_view(
        app->view_dispatcher, 0, variable_item_list_get_view(app->variable_item_list));
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
    view_dispatcher_run(app->view_dispatcher);

    desktop_api_set_settings(app->desktop, &app->settings);

    view_dispatcher_remove_view(app->view_dispatcher, 0);
    variable_item_list_free(app->variable_item_list);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_DESKTOP);
    furi_record_close(RECORD_GUI);
    free(app);

    return 0;
}
