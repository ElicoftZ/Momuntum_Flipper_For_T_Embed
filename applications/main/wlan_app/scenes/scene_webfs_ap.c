/* Dedicated-AP settings (VariableItemList, like the Evil Portal menu):
 * SSID / Password rows show their value on the right and open a TextInput on OK;
 * "Start Filesystem" hands over to the info scene in AP mode. Values live in
 * app->webfs_ssid/pw (loaded from /ext/webfs/config.txt on first enter). */

#include "../wlan_app.h"

enum {
    WebFsApItemSsid,
    WebFsApItemPassword,
    WebFsApItemStart,
};

static void webfs_ap_enter_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    uint32_t ev;
    switch(index) {
    case WebFsApItemSsid:
        ev = WlanAppCustomEventWebFsApSsid;
        break;
    case WebFsApItemPassword:
        ev = WlanAppCustomEventWebFsApPassword;
        break;
    default:
        ev = WlanAppCustomEventWebFsApStart;
        break;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, ev);
}

void wlan_app_scene_webfs_ap_on_enter(void* context) {
    WlanApp* app = context;
    if(app->webfs_ssid[0] == '\0') {
        wlan_webfs_config_load(app->webfs_ssid, app->webfs_pw);
    }

    VariableItemList* vil = app->variable_item_list;
    variable_item_list_reset(vil);

    VariableItem* item;
    item = variable_item_list_add(vil, "SSID", 1, NULL, app);
    variable_item_set_current_value_text(item, app->webfs_ssid);

    item = variable_item_list_add(vil, "Password", 1, NULL, app);
    variable_item_set_current_value_text(item, app->webfs_pw[0] ? app->webfs_pw : "(open)");

    variable_item_list_add(vil, "Start Filesystem", 1, NULL, app);

    variable_item_list_set_enter_callback(vil, webfs_ap_enter_cb, app);
    variable_item_list_set_selected_item(
        vil,
        (uint8_t)scene_manager_get_scene_state(app->scene_manager, WlanAppSceneWebFsAp));

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewVariableItemList);
}

bool wlan_app_scene_webfs_ap_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    SceneManager* sm = app->scene_manager;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventWebFsApSsid) {
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsAp, WebFsApItemSsid);
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsInput, 0 /* SSID */);
            scene_manager_next_scene(sm, WlanAppSceneWebFsInput);
            consumed = true;
        } else if(event.event == WlanAppCustomEventWebFsApPassword) {
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsAp, WebFsApItemPassword);
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsInput, 1 /* password */);
            scene_manager_next_scene(sm, WlanAppSceneWebFsInput);
            consumed = true;
        } else if(event.event == WlanAppCustomEventWebFsApStart) {
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsAp, WebFsApItemStart);
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsInfo, 1 /* AP */);
            scene_manager_next_scene(sm, WlanAppSceneWebFsInfo);
            consumed = true;
        }
    }
    return consumed;
}

void wlan_app_scene_webfs_ap_on_exit(void* context) {
    WlanApp* app = context;
    variable_item_list_reset(app->variable_item_list);
}
