#include "marauder_app_i.h"

static bool marauder_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    MarauderApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool marauder_app_back_event_callback(void* context) {
    furi_assert(context);
    MarauderApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static MarauderApp* marauder_app_alloc(void) {
    MarauderApp* app = calloc(1, sizeof(MarauderApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->scene_manager = scene_manager_alloc(&marauder_scene_handlers, app);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, marauder_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, marauder_app_back_event_callback);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, MarauderAppViewSubmenu, submenu_get_view(app->submenu));

    app->widget = widget_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, MarauderAppViewWidget, widget_get_view(app->widget));

    return app;
}

static void marauder_app_free(MarauderApp* app) {
    furi_assert(app);

    view_dispatcher_remove_view(app->view_dispatcher, MarauderAppViewSubmenu);
    submenu_free(app->submenu);

    view_dispatcher_remove_view(app->view_dispatcher, MarauderAppViewWidget);
    widget_free(app->widget);

    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    furi_record_close(RECORD_GUI);

    free(app);
}

int32_t marauder_app(void* p) {
    UNUSED(p);
    MarauderApp* app = marauder_app_alloc();

    scene_manager_next_scene(app->scene_manager, MarauderSceneMain);
    view_dispatcher_run(app->view_dispatcher);

    marauder_app_free(app);
    return 0;
}
