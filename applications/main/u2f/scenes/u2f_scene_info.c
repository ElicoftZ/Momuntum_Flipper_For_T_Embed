#include "../u2f_app_i.h"
#include "../ctap2_pin.h"
#include "../ctap2_rk.h"

#include <stdio.h>
#include <string.h>

/* A read-only summary of what this board currently answers a host with --
 * the same facts authenticatorGetInfo reports, in the terms a person asks the
 * question in: which FIDO generations it speaks, and whether a PIN gate is up.
 * Nothing here can be changed from this screen; Settings already owns that. */

static void u2f_scene_info_button_callback(GuiButtonType type, InputType input, void* context) {
    furi_assert(context);
    U2fApp* app = context;
    if(input != InputTypeShort) return;
    if(type == GuiButtonTypeLeft)
        view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventErrorBack);
}

void u2f_scene_info_on_enter(void* context) {
    U2fApp* app = context;

    const bool pin_set = ctap2_pin_is_set();
    char pin_line[32];
    if(!pin_set) {
        snprintf(pin_line, sizeof(pin_line), "PIN protection: Off");
    } else {
        uint8_t retries = ctap2_pin_get_retries();
        if(retries == 0) {
            snprintf(pin_line, sizeof(pin_line), "PIN protection: On (blocked)");
        } else {
            snprintf(pin_line, sizeof(pin_line), "PIN protection: On");
        }
    }

    char text[192];
    snprintf(
        text,
        sizeof(text),
        "\e#Security Key Info\e#\n"
        "Speaks: FIDO2 (CTAP2) + FIDO/U2F (CTAP1)\n"
        "%s\n"
        "Passkeys stored: %u",
        pin_line,
        (unsigned)ctap2_rk_count_all());

    widget_reset(app->widget);
    widget_add_text_box_element(app->widget, 0, 0, 128, 48, AlignCenter, AlignTop, text, false);
    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Back", u2f_scene_info_button_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, U2fAppViewError);
}

bool u2f_scene_info_on_event(void* context, SceneManagerEvent event) {
    U2fApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == U2fCustomEventErrorBack) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }

    return consumed;
}

void u2f_scene_info_on_exit(void* context) {
    U2fApp* app = context;
    widget_reset(app->widget);
}
