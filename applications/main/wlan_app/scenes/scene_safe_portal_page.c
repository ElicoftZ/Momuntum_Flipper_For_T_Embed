/* Safe Portal page picker (Submenu): lists every .html file in
 * /ext/safe_portal, scrolled with up/down and chosen with OK -- the same
 * folder-scan idea as the Evil Portal template picker, but as a submenu so it
 * works with only the wheel + OK on this board (no left/right, no blocking
 * file-browser modal). Selecting one stores /ext/safe_portal/<name> in
 * app->safe_portal_page and persists it, then returns to the settings scene. */

#include "../wlan_app.h"

#include <storage/storage.h>
#include <string.h>

#define SAFE_PORTAL_DIR    "/ext/safe_portal"
#define SAFE_PORTAL_MAX    24 /* submenu entries we're willing to list */
#define SAFE_PORTAL_NAME   64 /* max filename length, incl. ".html" + NUL */

/* The scene is single-instance and non-reentrant, so a file-scoped table is
 * enough to map the selected submenu index back to a filename. */
static char s_names[SAFE_PORTAL_MAX][SAFE_PORTAL_NAME];
static uint32_t s_count;

static bool ends_with_html(const char* name) {
    size_t n = strlen(name);
    if(n < 6) return false; /* "x.html" */
    const char* t = name + n - 5;
    return t[0] == '.' && (t[1] == 'h' || t[1] == 'H') && (t[2] == 't' || t[2] == 'T') &&
           (t[3] == 'm' || t[3] == 'M') && (t[4] == 'l' || t[4] == 'L');
}

static void safe_portal_page_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    if(index >= s_count) return; /* the "no files" placeholder */
    snprintf(
        app->safe_portal_page, sizeof(app->safe_portal_page), "%s/%s", SAFE_PORTAL_DIR,
        s_names[index]);
    wlan_webfs_safe_page_save(app->safe_portal_page);
    view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventSafePortalPagePicked);
}

void wlan_app_scene_safe_portal_page_on_enter(void* context) {
    WlanApp* app = context;
    s_count = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, SAFE_PORTAL_DIR)) {
        char name[SAFE_PORTAL_NAME];
        FileInfo info;
        while(s_count < SAFE_PORTAL_MAX && storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            if(!ends_with_html(name)) continue;
            strncpy(s_names[s_count], name, SAFE_PORTAL_NAME - 1);
            s_names[s_count][SAFE_PORTAL_NAME - 1] = '\0';
            s_count++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);

    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, "Pick a page");
    if(s_count == 0) {
        /* index SAFE_PORTAL_MAX is out of the valid range, so the callback
         * treats it as the placeholder and just lets Back return. */
        submenu_add_item(
            app->submenu, "No .html in safe_portal", SAFE_PORTAL_MAX, safe_portal_page_cb, app);
    } else {
        for(uint32_t i = 0; i < s_count; i++) {
            submenu_add_item(app->submenu, s_names[i], i, safe_portal_page_cb, app);
        }
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_safe_portal_page_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == WlanAppCustomEventSafePortalPagePicked) {
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }
    return consumed;
}

void wlan_app_scene_safe_portal_page_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
}
