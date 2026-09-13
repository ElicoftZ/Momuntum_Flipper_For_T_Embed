/* Safe Portal settings (VariableItemList, like scene_webfs_ap.c):
 * Name opens a TextInput on OK; Page opens a submenu listing the .html files
 * in /ext/safe_portal (up/down + OK, like the Evil Portal template picker);
 * "Start" hands over to the Web-FS info scene in safe mode (mode 2). Both are
 * app->safe_portal_ssid/page (loaded from /ext/webfs/safe_config.txt on first
 * enter) -- separate from webfs_ssid/pw so this never touches the real Web-FS
 * dedicated-AP config. */

#include "../wlan_app.h"

#include <string.h>

enum {
    SafePortalItemSsid,
    SafePortalItemPage,
    SafePortalItemStart,
};

static void safe_portal_enter_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    uint32_t ev;
    switch(index) {
    case SafePortalItemSsid:
        ev = WlanAppCustomEventSafePortalSsid;
        break;
    case SafePortalItemPage:
        ev = WlanAppCustomEventSafePortalPage;
        break;
    default:
        ev = WlanAppCustomEventSafePortalStart;
        break;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, ev);
}

/* Show only the file's basename in the row, not the whole /ext path. */
static const char* safe_portal_page_label(const char* path) {
    const char* slash = strrchr(path, '/');
    return (slash && slash[1]) ? slash + 1 : path;
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
    variable_item_set_current_value_text(item, safe_portal_page_label(app->safe_portal_page));

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
        } else if(event.event == WlanAppCustomEventSafePortalPage) {
            scene_manager_set_scene_state(sm, WlanAppSceneSafePortal, SafePortalItemPage);
            scene_manager_next_scene(sm, WlanAppSceneSafePortalPage);
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
