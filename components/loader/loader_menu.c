#include <gui.h>
#include <view_dispatcher.h>
#include <menu.h>
#include <submenu.h>
#include <icon_i.h>
#include <assets_icons.h>
#include <applications.h>
#include <archive/helpers/archive_favorites.h>
#include <archive/helpers/archive_files.h>
#include <core/dangerous_defines.h>
#include <flipper_application/flipper_application.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>
#include <m-list.h>
#include <esp_rom_sys.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "loader.h"
#include "loader_menu.h"

#define TAG "LoaderMenu"

typedef struct LoaderMenuApp LoaderMenuApp;

struct LoaderMenu {
    FuriThread* thread;
    void (*closed_cb)(void*);
    void* context;
    /* Set once the menu thread has built its views, so a reload arriving
     * before that is simply dropped — the menu is about to be built anyway. */
    LoaderMenuApp* app;
};

static void loader_menu_trace(const char* step) {
    esp_rom_printf(
        "\r\n[LM] %s free=%u\r\n",
        step,
        (unsigned)furi_thread_get_stack_space(furi_thread_get_current_id()));
}

static void loader_menu_trace_settings_registry(void) {
    esp_rom_printf(
        "\r\n[LM] settings_counts internal=%u external=%u\r\n",
        (unsigned)FLIPPER_SETTINGS_APPS_COUNT,
        (unsigned)FLIPPER_EXTSETTINGS_APPS_COUNT);

    if(FLIPPER_SETTINGS_APPS_COUNT == 0) {
        esp_rom_printf("\r\n[LM] settings_internal none\r\n");
    } else {
        for(size_t i = 0; i < FLIPPER_SETTINGS_APPS_COUNT; ++i) {
            esp_rom_printf(
                "\r\n[LM] settings_internal[%u] appid=%s name=%s\r\n",
                (unsigned)i,
                FLIPPER_SETTINGS_APPS[i].appid ? FLIPPER_SETTINGS_APPS[i].appid : "(null)",
                FLIPPER_SETTINGS_APPS[i].name ? FLIPPER_SETTINGS_APPS[i].name : "(null)");
        }
    }

    if(FLIPPER_EXTSETTINGS_APPS_COUNT == 0) {
        esp_rom_printf("\r\n[LM] settings_external none\r\n");
    } else {
        for(size_t i = 0; i < FLIPPER_EXTSETTINGS_APPS_COUNT; ++i) {
            esp_rom_printf(
                "\r\n[LM] settings_external[%u] launch=%s name=%s\r\n",
                (unsigned)i,
                FLIPPER_EXTSETTINGS_APPS[i].path ? FLIPPER_EXTSETTINGS_APPS[i].path : "(null)",
                FLIPPER_EXTSETTINGS_APPS[i].name ? FLIPPER_EXTSETTINGS_APPS[i].name : "(null)");
        }
    }
}

static int32_t loader_menu_thread(void* p);

LoaderMenu* loader_menu_alloc(void (*closed_cb)(void*), void* context) {
    LoaderMenu* loader_menu = malloc(sizeof(LoaderMenu));
    loader_menu->closed_cb = closed_cb;
    loader_menu->context = context;
    loader_menu->thread =
        furi_thread_alloc_ex(TAG, 4096, loader_menu_thread, loader_menu);
    furi_thread_start(loader_menu->thread);
    return loader_menu;
}

void loader_menu_free(LoaderMenu* loader_menu) {
    furi_assert(loader_menu);
    furi_thread_join(loader_menu->thread);
    furi_thread_free(loader_menu->thread);
    free(loader_menu);
}

typedef enum {
    LoaderMenuViewPrimary,
    LoaderMenuViewSettings,
} LoaderMenuView;

/* One row of the primary menu. A built-in app points name, launch and icon
 * into flash; a file entry allocates name and launch. The icon is tracked
 * separately because a file entry only owns one when its FAP manifest supplied
 * it — a file with no manifest falls back to a flash icon. launch is stored
 * rather than derived because internal apps are started by display name and
 * external ones by appid. */
typedef struct {
    const char* name;
    const Icon* icon;
    const char* launch;
    bool owns_strings;
    bool owns_icon;
} MenuApp;

LIST_DEF(MenuAppList, MenuApp, M_POD_OPLIST)
#define M_OPL_MenuAppList_t() LIST_OPLIST(MenuAppList)

struct LoaderMenuApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Menu* primary_menu;
    Submenu* settings_menu;
    MenuAppList_t apps_list;
    /* Raw contents of the layout file the menu was last built from, empty when
     * the default order is in use. A reload compares against it and rebuilds
     * only on a real change, so exiting an app that touched nothing does not
     * flicker the menu through an empty state. */
    FuriString* layout;
};

static void loader_menu_start(const char* name) {
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start_with_gui_error(loader, name, NULL);
    furi_record_close(RECORD_LOADER);
}

static void loader_menu_apps_callback(void* context, uint32_t index) {
    LoaderMenuApp* app = context;
    loader_menu_start(MenuAppList_get(app->apps_list, index)->launch);
}

static void loader_menu_pinned_callback(void* context, uint32_t index) {
    UNUSED(context);
    loader_menu_start(FLIPPER_EXTERNAL_APPS[index].path);
}

static void loader_menu_applications_callback(void* context, uint32_t index) {
    UNUSED(index);
    UNUSED(context);
    loader_menu_trace("apps_callback");
    loader_menu_start(LOADER_APPLICATIONS_NAME);
}

static void
    loader_menu_settings_menu_callback(void* context, InputType input_type, uint32_t index) {
    UNUSED(context);
    if(input_type == InputTypeShort) {
        loader_menu_start((const char*)index);
    } else if(input_type == InputTypeLong) {
        archive_favorites_handle_setting_pin_unpin((const char*)index, NULL);
    }
}

static void loader_menu_switch_to_settings(void* context, uint32_t index) {
    UNUSED(index);
    LoaderMenuApp* app = context;
    view_dispatcher_switch_to_view(app->view_dispatcher, LoaderMenuViewSettings);
}

static uint32_t loader_menu_switch_to_primary(void* context) {
    UNUSED(context);
    return LoaderMenuViewPrimary;
}

static uint32_t loader_menu_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

bool loader_menu_load_fap_meta(
    Storage* storage,
    FuriString* path,
    FuriString* name,
    const Icon** icon) {
    *icon = NULL;
    uint8_t* icon_buf = malloc(FAP_MANIFEST_MAX_ICON_SIZE);
    if(!flipper_application_load_name_and_icon(path, storage, &icon_buf, name)) {
        free(icon_buf);
        return false;
    }

    Icon* loaded = malloc(sizeof(Icon));
    FURI_CONST_ASSIGN(loaded->frame_count, 1);
    FURI_CONST_ASSIGN(loaded->frame_rate, 1);
    FURI_CONST_ASSIGN(loaded->width, 10);
    FURI_CONST_ASSIGN(loaded->height, 10);
    FURI_CONST_ASSIGN_PTR(loaded->frames, malloc(sizeof(const uint8_t*)));
    FURI_CONST_ASSIGN_PTR(loaded->frames[0], icon_buf);
    *icon = loaded;
    return true;
}

void loader_menu_free_fap_icon(const Icon* icon) {
    if(!icon) return;
    free((void*)icon->frames[0]);
    free((void*)icon->frames);
    free((void*)icon);
}

/* Index of the pinned app in FLIPPER_EXTERNAL_APPS, or the count if this build
 * does not include it. */
static size_t loader_menu_pinned_index(void) {
    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; i++) {
        if(!strcmp(FLIPPER_EXTERNAL_APPS[i].path, MAINMENU_PINNED_APPID)) return i;
    }
    return FLIPPER_EXTERNAL_APPS_COUNT;
}

static void loader_menu_add_app_entry(
    LoaderMenuApp* app,
    const char* name,
    const Icon* icon,
    const char* launch,
    bool owns_strings,
    bool owns_icon) {
    MenuAppList_push_back(app->apps_list, (MenuApp){name, icon, launch, owns_strings, owns_icon});
    menu_add_item(
        app->primary_menu,
        name,
        icon,
        MenuAppList_size(app->apps_list) - 1,
        loader_menu_apps_callback,
        app);
}

static const Icon* loader_menu_get_file_icon(const char* path) {
    const char* extension = strrchr(path, '.');
    if(extension && !strcasecmp(extension, ".js")) return &I_js_script_10px;
    return &I_file_10px;
}

static void loader_menu_find_add_app(
    LoaderMenuApp* app,
    Storage* storage,
    FuriString* line,
    size_t pinned) {
    if(furi_string_start_with(line, "/")) {
        const char* launch = strdup(furi_string_get_cstr(line));
        const Icon* icon;
        /* line doubles as the label output: on success it becomes the manifest
         * name, otherwise it still holds the path we fall back to. */
        const bool owns_icon = loader_menu_load_fap_meta(storage, line, line, &icon);
        if(!owns_icon) icon = loader_menu_get_file_icon(launch);
        loader_menu_add_app_entry(
            app, strdup(furi_string_get_cstr(line)), icon, launch, true, owns_icon);
        return;
    }

    for(size_t i = 0; i < FLIPPER_APPS_COUNT; i++) {
        if(furi_string_equal(line, FLIPPER_APPS[i].name)) {
            loader_menu_add_app_entry(
                app, FLIPPER_APPS[i].name, FLIPPER_APPS[i].icon, FLIPPER_APPS[i].name, false, false);
            return;
        }
    }

    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; i++) {
        if(i == pinned) continue;
        /* A saved custom menu must not resurrect a hidden entry. */
        if(furi_string_equal(line, FLIPPER_EXTERNAL_APPS[i].name)) {
            loader_menu_add_app_entry(
                app,
                FLIPPER_EXTERNAL_APPS[i].name,
                FLIPPER_EXTERNAL_APPS[i].icon,
                FLIPPER_EXTERNAL_APPS[i].path,
                false,
                false);
            return;
        }
    }
}

static void loader_menu_build_default(LoaderMenuApp* app, size_t pinned) {
    for(size_t i = 0; i < FLIPPER_APPS_COUNT; i++) {
        loader_menu_add_app_entry(
            app, FLIPPER_APPS[i].name, FLIPPER_APPS[i].icon, FLIPPER_APPS[i].name, false, false);
    }
    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; i++) {
        if(i == pinned) continue;
        loader_menu_add_app_entry(
            app,
            FLIPPER_EXTERNAL_APPS[i].name,
            FLIPPER_EXTERNAL_APPS[i].icon,
            FLIPPER_EXTERNAL_APPS[i].path,
            false,
            false);
    }
}

static void loader_menu_build_menu(LoaderMenuApp* app, LoaderMenu* menu) {
    UNUSED(menu);
    const size_t pinned = loader_menu_pinned_index();

    menu_add_item(
        app->primary_menu,
        LOADER_APPLICATIONS_NAME,
        &A_Plugins_14,
        0,
        loader_menu_applications_callback,
        app);

    MenuAppList_init(app->apps_list);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();
    unsigned long version;

    furi_string_reset(app->layout);

    if(file_stream_open(stream, MAINMENU_APPS_PATH, FSAM_READ, FSOM_OPEN_EXISTING) &&
       stream_read_line(stream, line) &&
       sscanf(furi_string_get_cstr(line), MAINMENU_APPS_HEADER_FMT, &version) == 1 &&
       version == MAINMENU_APPS_VERSION) {
        while(stream_read_line(stream, line)) {
            furi_string_trim(line);
            if(!furi_string_size(line)) continue;
            /* Recorded before the call below, which reuses line as its label
             * output for path entries. */
            furi_string_cat(app->layout, line);
            furi_string_cat_str(app->layout, "\n");
            loader_menu_find_add_app(app, storage, line, pinned);
        }
    } else {
        loader_menu_build_default(app, pinned);
    }

    furi_string_free(line);
    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);

    if(pinned < FLIPPER_EXTERNAL_APPS_COUNT) {
        menu_add_item(
            app->primary_menu,
            FLIPPER_EXTERNAL_APPS[pinned].name,
            FLIPPER_EXTERNAL_APPS[pinned].icon,
            pinned,
            loader_menu_pinned_callback,
            app);
    }

    if((FLIPPER_EXTSETTINGS_APPS_COUNT > 0) || (FLIPPER_SETTINGS_APPS_COUNT > 0)) {
        menu_add_item(
            app->primary_menu, "Settings", &A_Settings_14, 0, loader_menu_switch_to_settings, app);
    }
}

static void loader_menu_free_entries(LoaderMenuApp* app) {
    /* A built-in entry points at flash and must not be freed. */
    for
        M_EACH(menu_app, app->apps_list, MenuAppList_t) {
            if(menu_app->owns_icon) loader_menu_free_fap_icon(menu_app->icon);
            if(menu_app->owns_strings) {
                free((void*)menu_app->name);
                free((void*)menu_app->launch);
            }
        }
    MenuAppList_clear(app->apps_list);
}

/* Reads the layout file the way loader_menu_build_menu() records it, so the two
 * can be compared. Leaves out empty when the file is missing or its header does
 * not match, which is also what selects the default order. */
static void loader_menu_read_layout(Storage* storage, FuriString* out) {
    furi_string_reset(out);

    Stream* stream = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();
    unsigned long version;

    if(file_stream_open(stream, MAINMENU_APPS_PATH, FSAM_READ, FSOM_OPEN_EXISTING) &&
       stream_read_line(stream, line) &&
       sscanf(furi_string_get_cstr(line), MAINMENU_APPS_HEADER_FMT, &version) == 1 &&
       version == MAINMENU_APPS_VERSION) {
        while(stream_read_line(stream, line)) {
            furi_string_trim(line);
            if(!furi_string_size(line)) continue;
            furi_string_cat(out, line);
            furi_string_cat_str(out, "\n");
        }
    }

    furi_string_free(line);
    file_stream_close(stream);
    stream_free(stream);
}

void loader_menu_reload(LoaderMenu* loader_menu) {
    if(!loader_menu || !loader_menu->app) return;
    LoaderMenuApp* app = loader_menu->app;

    /* Exiting an app is the common case and almost never changes the layout;
     * rebuilding regardless would flicker the menu through an empty state
     * every time. */
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* layout = furi_string_alloc();
    loader_menu_read_layout(storage, layout);
    const bool unchanged = furi_string_equal(layout, app->layout);
    furi_string_free(layout);
    furi_record_close(RECORD_STORAGE);

    if(unchanged) return;

    const uint32_t selected = menu_get_selected_item(app->primary_menu);

    /* menu_reset() stops the icon animations built over the entry icons, so it
     * has to run before those icons are freed. */
    menu_reset(app->primary_menu);
    loader_menu_free_entries(app);
    loader_menu_build_menu(app, loader_menu);

    menu_set_selected_item(app->primary_menu, selected);
}

static void loader_menu_build_submenu(LoaderMenuApp* app, LoaderMenu* loader_menu) {
    for(size_t i = 0; i < FLIPPER_EXTSETTINGS_APPS_COUNT; i++) {
        submenu_add_item_ex(
            app->settings_menu,
            FLIPPER_EXTSETTINGS_APPS[i].name,
            (uint32_t)FLIPPER_EXTSETTINGS_APPS[i].path,
            loader_menu_settings_menu_callback,
            loader_menu);
    }
    for(size_t i = 0; i < FLIPPER_SETTINGS_APPS_COUNT; i++) {
        submenu_add_item_ex(
            app->settings_menu,
            FLIPPER_SETTINGS_APPS[i].name,
            (uint32_t)FLIPPER_SETTINGS_APPS[i].name,
            loader_menu_settings_menu_callback,
            loader_menu);
    }
}

static LoaderMenuApp* loader_menu_app_alloc(LoaderMenu* loader_menu) {
    LoaderMenuApp* app = malloc(sizeof(LoaderMenuApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->primary_menu = menu_alloc();
    app->settings_menu = submenu_alloc();
    app->layout = furi_string_alloc();

    loader_menu_trace_settings_registry();
    loader_menu_build_menu(app, loader_menu);
    loader_menu_build_submenu(app, loader_menu);

    // Primary menu
    View* primary_view = menu_get_view(app->primary_menu);
    view_set_context(primary_view, app->primary_menu);
    view_set_previous_callback(primary_view, loader_menu_exit);
    view_dispatcher_add_view(
        app->view_dispatcher, LoaderMenuViewPrimary, primary_view);

    // Settings menu
    View* settings_view = submenu_get_view(app->settings_menu);
    view_set_context(settings_view, app->settings_menu);
    view_set_previous_callback(settings_view, loader_menu_switch_to_primary);
    view_dispatcher_add_view(
        app->view_dispatcher, LoaderMenuViewSettings, settings_view);
    view_dispatcher_switch_to_view(app->view_dispatcher, LoaderMenuViewPrimary);

    return app;
}

static void loader_menu_app_free(LoaderMenuApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewPrimary);
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewSettings);
    view_dispatcher_free(app->view_dispatcher);

    loader_menu_free_entries(app);
    furi_string_free(app->layout);

    menu_free(app->primary_menu);
    submenu_free(app->settings_menu);
    furi_record_close(RECORD_GUI);
    free(app);
}

static int32_t loader_menu_thread(void* p) {
    LoaderMenu* loader_menu = p;
    furi_assert(loader_menu);
    loader_menu_trace("thread_start");

    LoaderMenuApp* app = loader_menu_app_alloc(loader_menu);
    loader_menu->app = app;
    loader_menu_trace("app_alloc");

    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    loader_menu_trace("attached");
    view_dispatcher_run(app->view_dispatcher);
    loader_menu_trace("dispatcher_return");

    loader_menu->app = NULL;

    if(loader_menu->closed_cb) {
        loader_menu->closed_cb(loader_menu->context);
    }

    loader_menu_app_free(app);
    loader_menu_trace("thread_end");

    return 0;
}
