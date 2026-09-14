#include "../u2f_app_i.h"
#include "../ctap2_pin.h"
#include "../ctap2_rk.h"

#include <stdio.h>

static void u2f_scene_settings_submenu_callback(void* context, uint32_t index) {
    furi_assert(context);
    U2fApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void u2f_scene_settings_on_enter(void* context) {
    U2fApp* app = context;

    const bool pin_set = ctap2_pin_is_set();

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "U2F Settings");

    /* Rebuilt on every entry rather than updated in place: a PIN can be set,
     * and credentials registered, from the host while this screen is not
     * showing, so anything cached here would be a lie. */
    snprintf(
        app->label_pin,
        sizeof(app->label_pin),
        pin_set ? "PIN protection: On" : "PIN protection: Off");
    submenu_add_item(
        app->submenu,
        app->label_pin,
        U2fSettingsIndexPin,
        u2f_scene_settings_submenu_callback,
        app);

    if(pin_set) {
        submenu_add_item(
            app->submenu,
            "Remove PIN",
            U2fSettingsIndexRemovePin,
            u2f_scene_settings_submenu_callback,
            app);

        uint8_t retries = ctap2_pin_get_retries();
        if(retries == 0) {
            snprintf(app->label_retries, sizeof(app->label_retries), "PIN blocked - reset needed");
        } else {
            snprintf(app->label_retries, sizeof(app->label_retries), "Tries left: %u", retries);
        }
        submenu_add_item(
            app->submenu,
            app->label_retries,
            U2fSettingsIndexRetries,
            u2f_scene_settings_submenu_callback,
            app);
    }

    snprintf(
        app->label_creds,
        sizeof(app->label_creds),
        "Saved sites (%u)",
        (unsigned)ctap2_rk_count_all());
    submenu_add_item(
        app->submenu,
        app->label_creds,
        U2fSettingsIndexCreds,
        u2f_scene_settings_submenu_callback,
        app);

    submenu_add_item(
        app->submenu,
        "Check security key",
        U2fSettingsIndexInfo,
        u2f_scene_settings_submenu_callback,
        app);

    submenu_add_item(
        app->submenu,
        "Scan a security key",
        U2fSettingsIndexScan,
        u2f_scene_settings_submenu_callback,
        app);

    submenu_add_item(
        app->submenu,
        "Reset authenticator",
        U2fSettingsIndexReset,
        u2f_scene_settings_submenu_callback,
        app);

    view_dispatcher_switch_to_view(app->view_dispatcher, U2fAppViewSettings);
}

bool u2f_scene_settings_on_event(void* context, SceneManagerEvent event) {
    U2fApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;
        switch(event.event) {
        case U2fSettingsIndexPin:
            /* One row does both jobs: with no PIN set it creates one, with a
             * PIN set it changes it -- which is also what a host's clientPIN
             * setPIN and changePIN do behind the same button. */
            app->pin_mode = ctap2_pin_is_set() ? U2fPinModeChange : U2fPinModeSet;
            scene_manager_next_scene(app->scene_manager, U2fScenePin);
            break;

        case U2fSettingsIndexRemovePin:
            app->pin_mode = U2fPinModeRemove;
            scene_manager_next_scene(app->scene_manager, U2fScenePin);
            break;

        case U2fSettingsIndexCreds:
            scene_manager_next_scene(app->scene_manager, U2fSceneCreds);
            break;

        case U2fSettingsIndexRetries:
            /* Informational row. */
            break;

        case U2fSettingsIndexInfo:
            scene_manager_next_scene(app->scene_manager, U2fSceneInfo);
            break;

        case U2fSettingsIndexScan:
            scene_manager_next_scene(app->scene_manager, U2fSceneScan);
            break;

        case U2fSettingsIndexReset:
            scene_manager_next_scene(app->scene_manager, U2fSceneReset);
            break;

        default:
            consumed = false;
            break;
        }
    }

    return consumed;
}

void u2f_scene_settings_on_exit(void* context) {
    U2fApp* app = context;
    submenu_reset(app->submenu);
}
