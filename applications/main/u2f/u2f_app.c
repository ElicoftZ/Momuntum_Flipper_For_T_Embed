#include "u2f_app_i.h"
#include "u2f_data.h"
#include <furi.h>
#include <furi_hal.h>

static bool u2f_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    U2fApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool u2f_app_back_event_callback(void* context) {
    furi_assert(context);
    U2fApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void u2f_app_tick_event_callback(void* context) {
    furi_assert(context);
    U2fApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

U2fApp* u2f_app_alloc(void) {
    /* calloc: the struct now carries pointers and PIN buffers that are read
     * before anything assigns them. */
    U2fApp* app = calloc(1, sizeof(U2fApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    /* A security key has to stay awake and readable for as long as it is plugged
     * in: the host may take minutes to ask for a touch, and there is no input in
     * the meantime to reset the idle timers.
     *
     * The two do different jobs. Insomnia blocks DEEP SLEEP, which would power
     * the board down and drop USB entirely -- the host would see the key vanish
     * mid-ceremony. Enforcing the backlight blocks the DIM, which insomnia does
     * not (notification_display_timer dims regardless), and which would otherwise
     * hide the "Press OK" prompt exactly when it matters. */
    furi_hal_power_insomnia_enter();
    notification_message(app->notifications, &sequence_display_backlight_enforce_on);

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&u2f_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, u2f_app_tick_event_callback, 500);

    view_dispatcher_set_custom_event_callback(app->view_dispatcher, u2f_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, u2f_app_back_event_callback);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Custom Widget
    app->widget = widget_alloc();
    view_dispatcher_add_view(app->view_dispatcher, U2fAppViewError, widget_get_view(app->widget));

    app->u2f_view = u2f_view_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, U2fAppViewMain, u2f_view_get_view(app->u2f_view));

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, U2fAppViewSettings, submenu_get_view(app->submenu));

    app->pin_input = u2f_pin_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, U2fAppViewPinInput, u2f_pin_input_get_view(app->pin_input));

    if(furi_hal_usb_is_locked()) {
        app->error = U2fAppErrorCloseRpc;
        scene_manager_next_scene(app->scene_manager, U2fSceneError);
    } else {
        if(u2f_data_check(true)) {
            scene_manager_next_scene(app->scene_manager, U2fSceneMain);
        } else {
            app->error = U2fAppErrorNoFiles;
            scene_manager_next_scene(app->scene_manager, U2fSceneError);
        }
    }

    return app;
}

void u2f_app_free(U2fApp* app) {
    furi_assert(app);

    // Views
    view_dispatcher_remove_view(app->view_dispatcher, U2fAppViewPinInput);
    u2f_pin_input_free(app->pin_input);

    view_dispatcher_remove_view(app->view_dispatcher, U2fAppViewSettings);
    submenu_free(app->submenu);

    view_dispatcher_remove_view(app->view_dispatcher, U2fAppViewMain);
    u2f_view_free(app->u2f_view);

    // Custom Widget
    view_dispatcher_remove_view(app->view_dispatcher, U2fAppViewError);
    widget_free(app->widget);

    // View dispatcher
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    /* Release the display and sleep locks before dropping the record. */
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
    furi_hal_power_insomnia_exit();

    // Close records
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);

    free(app);
}

int32_t u2f_app(void* p) {
    UNUSED(p);
    U2fApp* u2f_app = u2f_app_alloc();

    view_dispatcher_run(u2f_app->view_dispatcher);

    u2f_app_free(u2f_app);

    return 0;
}
