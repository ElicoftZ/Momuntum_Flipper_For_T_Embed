#include "../wlan_app.h"
#include "../wlan_update_source.h"

// Persistente Wahl der Update-Quelle (siehe wlan_update_source.h), erreichbar
// aus dem Hauptmenü. Wird von scene_fw_update beim nächsten "Update"-Aufruf
// gelesen, nicht mehr pro Lauf abgefragt.

#define UPDATE_SOURCE_COUNT 2
static const char* const update_source_text[UPDATE_SOURCE_COUNT] = {"Momuntum", "Sor3nt"};

static void update_source_set_cb(VariableItem* item) {
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= UPDATE_SOURCE_COUNT) idx = UPDATE_SOURCE_COUNT - 1;
    variable_item_set_current_value_text(item, update_source_text[idx]);
    wlan_update_source_set_sor3nt(idx == 1);
}

void wlan_app_scene_update_source_settings_on_enter(void* context) {
    WlanApp* app = context;
    VariableItemList* list = app->variable_item_list;
    variable_item_list_reset(list);

    uint8_t idx = wlan_update_source_get_sor3nt() ? 1 : 0;
    VariableItem* it = variable_item_list_add(
        list, "Update Source", UPDATE_SOURCE_COUNT, update_source_set_cb, app);
    variable_item_set_current_value_index(it, idx);
    variable_item_set_current_value_text(it, update_source_text[idx]);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewVariableItemList);
}

bool wlan_app_scene_update_source_settings_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void wlan_app_scene_update_source_settings_on_exit(void* context) {
    WlanApp* app = context;
    variable_item_list_reset(app->variable_item_list);
}
