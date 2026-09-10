#include "../u2f_app_i.h"
#include "../ctap2_pin.h"

#include <gui/modules/widget.h>

/* Reset is destructive and irreversible, so it is deliberately two deliberate
 * presses away rather than one: a mis-scrolled OK in the settings list must not
 * be able to destroy every credential on the device. */

static void u2f_scene_reset_button_callback(GuiButtonType type, InputType input, void* context) {
    furi_assert(context);
    U2fApp* app = context;
    if(input != InputTypeShort) return;

    view_dispatcher_send_custom_event(
        app->view_dispatcher,
        (type == GuiButtonTypeRight) ? U2fCustomEventConfirm : U2fCustomEventErrorBack);
}

void u2f_scene_reset_on_enter(void* context) {
    U2fApp* app = context;

    widget_reset(app->widget);
    widget_add_text_box_element(
        app->widget,
        0,
        0,
        128,
        46,
        AlignCenter,
        AlignTop,
        "\e#Reset authenticator?\e#\nErases the PIN and makes every\nregistered site unusable.\nThis cannot be undone.",
        false);
    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Cancel", u2f_scene_reset_button_callback, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "Reset", u2f_scene_reset_button_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, U2fAppViewError);
}

bool u2f_scene_reset_on_event(void* context, SceneManagerEvent event) {
    U2fApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == U2fCustomEventConfirm) {
            ctap2_pin_erase();
            /* Credentials are wrapped under the device key rather than stored,
             * so replacing the key is what actually revokes them -- there is
             * no per-credential record to delete. */
            if(app->u2f_ready) u2f_regenerate_device_key(app->u2f_instance);
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;

        } else if(event.event == U2fCustomEventErrorBack) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }

    return consumed;
}

void u2f_scene_reset_on_exit(void* context) {
    U2fApp* app = context;
    widget_reset(app->widget);
}
