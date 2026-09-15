#include "marauder_app_i.h"

/* RGB888 -> RGB565 with the ESP32-S3 SPI byte swap. Same formula as
 * color_picker_pack_swap() in notification_settings_color_picker.c --
 * mirrored here rather than shared, since that one is file-local static. */
static inline uint16_t marauder_pack_swap(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
    return ((v & 0xFF) << 8) | (v >> 8);
}

/* Classic pentest-tool green, applied only while a Marauder screen is on
 * screen -- see saved_fg_color in marauder_app_i.h for the restore side. */
#define MARAUDER_INK_R 0x00
#define MARAUDER_INK_G 0xFF
#define MARAUDER_INK_B 0x00

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

    /* Tint scoped to this app: snapshot whatever the user's shell-color
     * setting currently has, apply Marauder's own ink color, and put the
     * snapshot back in marauder_app_free(). */
    app->saved_fg_color = furi_hal_display_get_fg_color();
    furi_hal_display_set_fg_color(
        marauder_pack_swap(MARAUDER_INK_R, MARAUDER_INK_G, MARAUDER_INK_B));

    return app;
}

static void marauder_app_free(MarauderApp* app) {
    furi_assert(app);

    furi_hal_display_set_fg_color(app->saved_fg_color);

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
