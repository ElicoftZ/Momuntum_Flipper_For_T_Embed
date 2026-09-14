#include "../u2f_app_i.h"
#include "../fido_nfc_scan.h"
#include "../views/u2f_view.h"

#include <furi_hal_usb_hid_u2f.h>

#include <stdio.h>
#include <string.h>

/* Inspecting someone ELSE's FIDO2/U2F security key -- the Flipper as an NFC
 * reader for once, instead of the tag it is everywhere else in this app.
 * Everything shown here is public capability info any reader gets for free
 * (authenticatorGetInfo), not something extracted from the key. */

/* Scene-state values, stored via scene_manager_set_scene_state. */
#define U2fScanUiSearching 0
#define U2fScanUiNotFido   1
#define U2fScanUiDone      2

static void u2f_scene_scan_button_callback(GuiButtonType type, InputType input, void* context) {
    furi_assert(context);
    U2fApp* app = context;
    if(input != InputTypeShort) return;

    view_dispatcher_send_custom_event(
        app->view_dispatcher,
        (type == GuiButtonTypeRight) ? U2fCustomEventConfirm : U2fCustomEventErrorBack);
}

static void u2f_scene_scan_show_searching(U2fApp* app) {
    widget_reset(app->widget);
    widget_add_string_multiline_element(
        app->widget,
        64,
        24,
        AlignCenter,
        AlignCenter,
        FontPrimary,
        "Hold their key to\nFlipper's back");
    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Cancel", u2f_scene_scan_button_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, U2fAppViewError);
}

static void u2f_scene_scan_show_not_fido(U2fApp* app) {
    widget_reset(app->widget);
    widget_add_text_box_element(
        app->widget,
        0,
        0,
        128,
        48,
        AlignCenter,
        AlignTop,
        "\e#Not a FIDO key\e#\nThat tag did not answer as a FIDO2/U2F security key.",
        false);
    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Back", u2f_scene_scan_button_callback, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "Retry", u2f_scene_scan_button_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, U2fAppViewError);
}

static void u2f_scene_scan_show_result(U2fApp* app, const FidoNfcScanResult* r) {
    const char* protocol;
    if(r->has_ctap2 && r->has_u2f) {
        protocol = "FIDO2 + FIDO/U2F(1)";
    } else if(r->has_ctap2) {
        protocol = "FIDO2 (CTAP2)";
    } else {
        protocol = "FIDO/U2F only (FIDO1)";
    }

    const char* pin;
    if(!r->has_ctap2) {
        pin = "PIN: n/a (U2F only)";
    } else if(!r->pin_capable) {
        pin = "PIN: not supported";
    } else if(r->pin_set) {
        pin = "PIN: set";
    } else {
        pin = "PIN: supported, not set";
    }

    /* The one-line takeaway the user actually asked for: is this thing
     * locked down, or not. A PIN is the only thing here that gates use of
     * the key by whoever is holding it -- resident-key support does not. */
    const char* verdict;
    if(r->has_ctap2 && r->pin_set) {
        verdict = "Verdict: PIN-protected";
    } else if(r->has_ctap2 && r->pin_capable) {
        verdict = "Verdict: NOT secure - no PIN set";
    } else {
        verdict = "Verdict: no PIN protection";
    }

    char text[256];
    int n = snprintf(text, sizeof(text), "\e#Security Key\e#\n%s\n%s", protocol, pin);
    if(r->has_ctap2 && n > 0 && (size_t)n < sizeof(text)) {
        n += snprintf(
            text + n,
            sizeof(text) - (size_t)n,
            "\nPasskeys: %s",
            r->resident_key ? "yes" : "no");
    }
    if(n > 0 && (size_t)n < sizeof(text)) {
        snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", verdict);
    }

    widget_reset(app->widget);
    widget_add_text_box_element(app->widget, 0, 0, 128, 48, AlignCenter, AlignTop, text, false);
    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Back", u2f_scene_scan_button_callback, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "Retry", u2f_scene_scan_button_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, U2fAppViewError);
}

static void u2f_scene_scan_restart(U2fApp* app) {
    if(app->nfc_scan != NULL) fido_nfc_scan_stop(app->nfc_scan);
    app->nfc_scan = fido_nfc_scan_start();
    scene_manager_set_scene_state(app->scene_manager, U2fSceneScan, U2fScanUiSearching);
    u2f_scene_scan_show_searching(app);
}

void u2f_scene_scan_on_enter(void* context) {
    U2fApp* app = context;

    /* Settings (and this scene under it) is reachable from the main screen's
     * Up/Down even while the board is emulating a key over NFC -- and the
     * radio cannot be a reader and a tag at once. Give up the emulated side
     * first, or nfc_alloc() below aborts the whole firmware fighting it for
     * the antenna. */
    if(app->nfc_mode) {
        if(app->fido_nfc != NULL) {
            fido_nfc_stop(app->fido_nfc);
            app->fido_nfc = NULL;
        }
        app->nfc_mode = false;
        /* So the main screen (reached again via Back, which resumes rather
         * than rebuilds it) does not keep showing the NFC prompt for a mode
         * this scene just silently turned off. */
        u2f_view_set_state(
            app->u2f_view, furi_hal_hid_u2f_is_connected() ? U2fMsgIdle : U2fMsgNotConnected);
    }

    u2f_scene_scan_restart(app);
}

bool u2f_scene_scan_on_event(void* context, SceneManagerEvent event) {
    U2fApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        /* Only worth polling while a scan is actually in flight -- once a
         * result is on screen it does not change until Retry starts a new
         * one, and re-reading it every tick would just waste cycles. */
        if(scene_manager_get_scene_state(app->scene_manager, U2fSceneScan) ==
           U2fScanUiSearching) {
            FidoNfcScanResult result;
            fido_nfc_scan_get_result(app->nfc_scan, &result);
            if(result.state == FidoNfcScanStateNotFido) {
                scene_manager_set_scene_state(app->scene_manager, U2fSceneScan, U2fScanUiNotFido);
                u2f_scene_scan_show_not_fido(app);
            } else if(result.state == FidoNfcScanStateDone) {
                scene_manager_set_scene_state(app->scene_manager, U2fSceneScan, U2fScanUiDone);
                u2f_scene_scan_show_result(app, &result);
            }
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == U2fCustomEventErrorBack) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        } else if(event.event == U2fCustomEventConfirm) {
            u2f_scene_scan_restart(app);
            consumed = true;
        }
    }

    return consumed;
}

void u2f_scene_scan_on_exit(void* context) {
    U2fApp* app = context;
    if(app->nfc_scan != NULL) {
        fido_nfc_scan_stop(app->nfc_scan);
        app->nfc_scan = NULL;
    }
    widget_reset(app->widget);
}
