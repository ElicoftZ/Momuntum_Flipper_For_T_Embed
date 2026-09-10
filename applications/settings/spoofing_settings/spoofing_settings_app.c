#include "spoofing_settings_app.h"

#include <namechanger/namechanger.h>
#include <flipper_format/flipper_format.h>
#include <power/power_service/power.h>

#include "scenes/spoofing_settings_scene.h"

static bool spoofing_settings_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    SpoofingSettingsApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool spoofing_settings_back_event_callback(void* context) {
    furi_assert(context);
    SpoofingSettingsApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

SpoofingSettingsApp* spoofing_settings_app_alloc(void) {
    SpoofingSettingsApp* app = malloc(sizeof(SpoofingSettingsApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&spoofing_settings_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, spoofing_settings_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, spoofing_settings_back_event_callback);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, SpoofingSettingsAppViewMenu, submenu_get_view(app->submenu));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        SpoofingSettingsAppViewTextInput,
        text_input_get_view(app->text_input));

    app->variable_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        SpoofingSettingsAppViewVariableItemList,
        variable_item_list_get_view(app->variable_item_list));

    app->popup = popup_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, SpoofingSettingsAppViewPopup, popup_get_view(app->popup));

    return app;
}

void spoofing_settings_app_free(SpoofingSettingsApp* app) {
    furi_assert(app);

    bool save_name = app->save_name;
    bool save_color = app->save_color;
    /* Reboot only after a successful NAME persist — a color-only change is
     * already live (RAM+BLE) and just needs to reach the SD for next boot. */
    bool name_saved = false;

    if(save_name || save_color) {
        Storage* storage = furi_record_open(RECORD_STORAGE);

        if(save_name && app->device_name[0] == '\0') {
            /* Empty name = reset to factory defaults (name + color): drop the
             * settings file so the next boot derives everything fresh. */
            name_saved = storage_simply_remove(storage, NAMECHANGER_PATH);
        } else {
            /* When only the color changed, keep the current effective name. */
            const char* name = (save_name && app->device_name[0]) ?
                                    app->device_name :
                                    furi_hal_version_get_name_ptr();

            /* NAMECHANGER_PATH = /ext/dolphin/name.settings — the dolphin dir is
             * not created anywhere on this port, so create it before writing.
             * Persistence lives on the SD, never NVS (an nvs_commit() on a
             * PSRAM-stacked app thread double-faults into a TG1WDT reset). */
            if(storage_simply_mkdir(storage, EXT_PATH("dolphin"))) {
                FlipperFormat* file = flipper_format_file_alloc(storage);

                bool ok = false;
                do {
                    if(!flipper_format_file_open_always(file, NAMECHANGER_PATH)) break;
                    if(!flipper_format_write_header_cstr(
                           file, NAMECHANGER_HEADER, NAMECHANGER_VERSION))
                        break;
                    if(!flipper_format_write_string_cstr(file, "Name", name)) break;
                    uint32_t color = (uint32_t)furi_hal_version_get_hw_color();
                    if(!flipper_format_write_uint32(file, "Color", &color, 1)) break;
                    ok = true;
                } while(0);

                flipper_format_free(file);
                if(save_name) name_saved = ok;
            }
        }
        furi_record_close(RECORD_STORAGE);
    }

    view_dispatcher_remove_view(app->view_dispatcher, SpoofingSettingsAppViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, SpoofingSettingsAppViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, SpoofingSettingsAppViewVariableItemList);
    view_dispatcher_remove_view(app->view_dispatcher, SpoofingSettingsAppViewPopup);
    submenu_free(app->submenu);
    text_input_free(app->text_input);
    variable_item_list_free(app->variable_item_list);
    popup_free(app->popup);

    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);
    furi_record_close(RECORD_GUI);
    free(app);

    if(name_saved) {
        Power* power = furi_record_open(RECORD_POWER);
        power_reboot(power, PowerBootModeNormal);
    }
}

int32_t spoofing_settings_app(void* p) {
    UNUSED(p);

    SpoofingSettingsApp* app = spoofing_settings_app_alloc();
    scene_manager_next_scene(app->scene_manager, SpoofingSettingsAppSceneStart);

    view_dispatcher_run(app->view_dispatcher);

    spoofing_settings_app_free(app);

    return 0;
}
