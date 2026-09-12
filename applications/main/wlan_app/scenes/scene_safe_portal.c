/* Safe Portal settings (VariableItemList, like scene_webfs_ap.c):
 * Name opens a TextInput on OK; Page opens a file browser to pick the .html
 * file to serve; "Start" hands over to the Web-FS info scene in safe mode
 * (mode 2). Both are app->safe_portal_ssid/page (loaded from
 * /ext/webfs/safe_config.txt on first enter) -- separate from webfs_ssid/pw
 * so this never touches the real Web-FS dedicated-AP config. */

#include "../wlan_app.h"
#include <dialogs/dialogs.h>

#include <string.h>

enum {
    SafePortalItemSsid,
    SafePortalItemPage,
    SafePortalItemStart,
};

/* dialog_file_browser_show is a blocking modal the Dialogs service draws
 * itself (like bad_usb_scene_file_select.c) -- it doesn't go through this
 * scene's own view, so it's called straight from the enter callback rather
 * than routed as a custom event. */
static void safe_portal_pick_page(WlanApp* app) {
    FuriString* path = furi_string_alloc_set(app->safe_portal_page);

    DialogsFileBrowserOptions opts;
    dialog_file_browser_set_basic_options(&opts, ".html", NULL);
    opts.base_path = "/ext/safe_portal";

    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    if(dialog_file_browser_show(dialogs, path, path, &opts)) {
        strncpy(app->safe_portal_page, furi_string_get_cstr(path), WLAN_SAFE_PORTAL_PAGE_MAX);
        app->safe_portal_page[WLAN_SAFE_PORTAL_PAGE_MAX] = '\0';
        wlan_webfs_safe_page_save(app->safe_portal_page);
    }
    furi_record_close(RECORD_DIALOGS);
    furi_string_free(path);
}

static void safe_portal_enter_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    if(index == SafePortalItemPage) {
        safe_portal_pick_page(app);
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventSafePortalPagePicked);
        return;
    }
    uint32_t ev = (index == SafePortalItemSsid) ? WlanAppCustomEventSafePortalSsid :
                                                   WlanAppCustomEventSafePortalStart;
    view_dispatcher_send_custom_event(app->view_dispatcher, ev);
}

void wlan_app_scene_safe_portal_on_enter(void* context) {
    WlanApp* app = context;
    if(app->safe_portal_ssid[0] == '\0') {
        wlan_webfs_safe_ssid_load(app->safe_portal_ssid);
    }
    if(app->safe_portal_page[0] == '\0') {
        wlan_webfs_safe_page_load(app->safe_portal_page);
    }

    VariableItemList* vil = app->variable_item_list;
    variable_item_list_reset(vil);

    VariableItem* item;
    item = variable_item_list_add(vil, "Name", 1, NULL, app);
    variable_item_set_current_value_text(item, app->safe_portal_ssid);

    item = variable_item_list_add(vil, "Page", 1, NULL, app);
    variable_item_set_current_value_text(item, app->safe_portal_page);

    variable_item_list_add(vil, "Start", 1, NULL, app);

    variable_item_list_set_enter_callback(vil, safe_portal_enter_cb, app);
    variable_item_list_set_selected_item(
        vil,
        (uint8_t)scene_manager_get_scene_state(app->scene_manager, WlanAppSceneSafePortal));

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewVariableItemList);
}

bool wlan_app_scene_safe_portal_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    SceneManager* sm = app->scene_manager;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventSafePortalSsid) {
            scene_manager_set_scene_state(sm, WlanAppSceneSafePortal, SafePortalItemSsid);
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsInput, 2 /* safe portal name */);
            scene_manager_next_scene(sm, WlanAppSceneWebFsInput);
            consumed = true;
        } else if(event.event == WlanAppCustomEventSafePortalPagePicked) {
            /* File browser already ran (blocking); just refresh the row. */
            variable_item_set_current_value_text(
                variable_item_list_get(app->variable_item_list, SafePortalItemPage),
                app->safe_portal_page);
            consumed = true;
        } else if(event.event == WlanAppCustomEventSafePortalStart) {
            scene_manager_set_scene_state(sm, WlanAppSceneSafePortal, SafePortalItemStart);
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsInfo, 2 /* safe portal */);
            scene_manager_next_scene(sm, WlanAppSceneWebFsInfo);
            consumed = true;
        }
    }
    return consumed;
}

void wlan_app_scene_safe_portal_on_exit(void* context) {
    WlanApp* app = context;
    variable_item_list_reset(app->variable_item_list);
}
