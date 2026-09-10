#include "../u2f_app_i.h"
#include "../ctap2_pin.h"

#include <string.h>

/* Prompt for the step the scene is on. */
static const char* u2f_scene_pin_header(const U2fApp* app) {
    switch(app->pin_stage) {
    case U2fPinStageCurrent:
        return "Enter current PIN";
    case U2fPinStageNew:
        return (app->pin_mode == U2fPinModeChange) ? "Enter new PIN" : "Enter new PIN";
    case U2fPinStageConfirm:
    default:
        return "Confirm new PIN";
    }
}

static void u2f_scene_pin_done_callback(const char* pin, void* context) {
    furi_assert(context);
    U2fApp* app = context;

    strncpy(app->pin_entry, pin, sizeof(app->pin_entry) - 1);
    app->pin_entry[sizeof(app->pin_entry) - 1] = '\0';

    /* The view is mid-input-callback; switching scenes has to happen off the
     * dispatcher, not from inside it. */
    view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventPinEntered);
}

static void u2f_scene_pin_back_callback(void* context) {
    furi_assert(context);
    U2fApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventPinCancelled);
}

void u2f_scene_pin_on_enter(void* context) {
    U2fApp* app = context;

    /* Change and Remove both start by proving the current PIN. Without that,
     * anyone who picks the board up could clear the PIN and keep the
     * credentials -- a reset at least destroys them. */
    app->pin_stage =
        (app->pin_mode == U2fPinModeSet) ? U2fPinStageNew : U2fPinStageCurrent;

    memset(app->pin_entry, 0, sizeof(app->pin_entry));
    memset(app->pin_first, 0, sizeof(app->pin_first));

    u2f_pin_input_set_callbacks(
        app->pin_input, u2f_scene_pin_done_callback, u2f_scene_pin_back_callback, app);
    u2f_pin_input_reset(app->pin_input, u2f_scene_pin_header(app));

    if(app->pin_stage == U2fPinStageCurrent && ctap2_pin_get_retries() == 0) {
        u2f_pin_input_set_error(app->pin_input, "PIN blocked - reset needed");
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, U2fAppViewPinInput);
}

/* Handle one completed entry. Returns true when the scene is finished. */
static bool u2f_scene_pin_advance(U2fApp* app) {
    switch(app->pin_stage) {
    case U2fPinStageCurrent: {
        /* Spends a retry on failure, exactly as the host path does -- letting
         * the on-device route guess for free would make the retry counter
         * protecting the host route pointless. */
        if(!ctap2_pin_check((const uint8_t*)app->pin_entry, strlen(app->pin_entry))) {
            uint8_t retries = ctap2_pin_get_retries();
            u2f_pin_input_set_error(
                app->pin_input, (retries == 0) ? "Blocked - reset needed" : "Wrong PIN");
            return false;
        }

        if(app->pin_mode == U2fPinModeRemove) {
            ctap2_pin_erase();
            return true;
        }

        app->pin_stage = U2fPinStageNew;
        u2f_pin_input_reset(app->pin_input, u2f_scene_pin_header(app));
        return false;
    }

    case U2fPinStageNew:
        memcpy(app->pin_first, app->pin_entry, sizeof(app->pin_first));
        app->pin_stage = U2fPinStageConfirm;
        u2f_pin_input_reset(app->pin_input, u2f_scene_pin_header(app));
        return false;

    case U2fPinStageConfirm:
    default:
        if(strcmp(app->pin_first, app->pin_entry) != 0) {
            /* Back to the first entry rather than the confirm -- re-confirming
             * against a PIN the user may have mistyped is worse than starting
             * the pair over. */
            app->pin_stage = U2fPinStageNew;
            memset(app->pin_first, 0, sizeof(app->pin_first));
            u2f_pin_input_reset(app->pin_input, u2f_scene_pin_header(app));
            u2f_pin_input_set_error(app->pin_input, "PINs did not match");
            return false;
        }

        ctap2_pin_store((const uint8_t*)app->pin_entry, strlen(app->pin_entry));
        return true;
    }
}

bool u2f_scene_pin_on_event(void* context, SceneManagerEvent event) {
    U2fApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == U2fCustomEventPinEntered) {
            bool finished = u2f_scene_pin_advance(app);
            /* Wipe before leaving either way: this buffer holds the PIN in
             * clear and the app struct outlives the scene. */
            memset(app->pin_entry, 0, sizeof(app->pin_entry));
            if(finished) {
                memset(app->pin_first, 0, sizeof(app->pin_first));
                scene_manager_previous_scene(app->scene_manager);
            }
            consumed = true;

        } else if(event.event == U2fCustomEventPinCancelled) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }

    return consumed;
}

void u2f_scene_pin_on_exit(void* context) {
    U2fApp* app = context;
    u2f_pin_input_set_callbacks(app->pin_input, NULL, NULL, NULL);
    memset(app->pin_entry, 0, sizeof(app->pin_entry));
    memset(app->pin_first, 0, sizeof(app->pin_first));
}
