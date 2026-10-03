#include "../nfc_app_i.h"
#include <furi_hal_nfc.h>

enum SubmenuIndex {
    SubmenuIndexOnboard,
    SubmenuIndexQwiic,
};

static void nfc_scene_nfc_source_submenu_callback(void* context, uint32_t index) {
    NfcApp* instance = context;
    view_dispatcher_send_custom_event(instance->view_dispatcher, index);
}

void nfc_scene_nfc_source_on_enter(void* context) {
    NfcApp* instance = context;
    Submenu* submenu = instance->submenu;

    bool using_qwiic = furi_hal_nfc_is_using_qwiic();

    submenu_add_item(
        submenu,
        using_qwiic ? "Onboard" : "Onboard (active)",
        SubmenuIndexOnboard,
        nfc_scene_nfc_source_submenu_callback,
        instance);
    submenu_add_item(
        submenu,
        using_qwiic ? "Qwiic (active)" : "Qwiic (PN532)",
        SubmenuIndexQwiic,
        nfc_scene_nfc_source_submenu_callback,
        instance);
    submenu_set_selected_item(submenu, using_qwiic ? SubmenuIndexQwiic : SubmenuIndexOnboard);

    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewMenu);
}

static void nfc_scene_nfc_source_popup_callback(void* context) {
    NfcApp* instance = context;
    view_dispatcher_send_custom_event(instance->view_dispatcher, NfcCustomEventViewExit);
}

/* Attempts the switch immediately on selection (rather than a separate
 * confirm step) and shows the real result -- a network/module that isn't
 * actually there should say so, not silently pretend it worked. Matches
 * furi_hal_nfc_set_use_qwiic()'s own contract: it re-runs the full PN532
 * handshake on the requested bus and reverts on failure, so `ok` here always
 * reflects what bus is ACTUALLY active afterward. */
bool nfc_scene_nfc_source_on_event(void* context, SceneManagerEvent event) {
    NfcApp* instance = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubmenuIndexOnboard || event.event == SubmenuIndexQwiic) {
            bool want_qwiic = event.event == SubmenuIndexQwiic;
            bool ok = furi_hal_nfc_set_use_qwiic(want_qwiic);

            Popup* popup = instance->popup;
            popup_set_header(
                popup,
                ok ? (want_qwiic ? "Using Qwiic" : "Using Onboard") : "No PN532 found",
                64,
                20,
                AlignCenter,
                AlignTop);
            if(!ok) {
                popup_set_text(
                    popup, "Check wiring, staying\non previous source", 64, 40, AlignCenter, AlignTop);
            }
            popup_set_timeout(popup, ok ? 1200 : 2000);
            popup_set_context(popup, instance);
            popup_set_callback(popup, nfc_scene_nfc_source_popup_callback);
            popup_enable_timeout(popup);
            view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewPopup);
            consumed = true;
        } else if(event.event == NfcCustomEventViewExit) {
            popup_reset(instance->popup);
            scene_manager_search_and_switch_to_previous_scene(
                instance->scene_manager, NfcSceneStart);
            consumed = true;
        }
    }

    return consumed;
}

void nfc_scene_nfc_source_on_exit(void* context) {
    NfcApp* instance = context;
    submenu_reset(instance->submenu);
}
