#pragma once
#include <furi.h>
#include <icon.h>
#include <storage/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Main menu layout, written by the Momentum app. After the header line comes
 * one entry per line, holding either the display name of an app in FLIPPER_APPS
 * or FLIPPER_EXTERNAL_APPS, or an absolute path to a .fap or .js on the SD
 * card. A line naming neither is skipped, so a layout saved by a build with
 * more apps still loads here. A missing or unreadable file means the default
 * order. */
#define MAINMENU_APPS_PATH       INT_PATH(".mainmenu_apps.txt")
#define MAINMENU_APPS_HEADER_FMT "MenuAppList Version %lu"
#define MAINMENU_APPS_VERSION    1UL

/* Always present in the menu and not editable, so removing every other entry
 * cannot strand the user without a way back to the menu editor. Momentum pins
 * the last external app instead, which works there because its settings app
 * sorts last; here app order comes from the .fam files and does not. */
#define MAINMENU_PINNED_APPID "momentum_app"

typedef struct LoaderMenu LoaderMenu;

LoaderMenu* loader_menu_alloc(void (*closed_cb)(void*), void* context);

void loader_menu_free(LoaderMenu* loader_menu);

/** Rebuild the primary menu from MAINMENU_APPS_PATH.
 *
 * The menu is built once when it is allocated and only torn down when the user
 * leaves it for the desktop, so launching an app and coming back would
 * otherwise show a layout from before the Momentum app edited it. Safe to call
 * from another thread: every Menu mutation takes the view's model lock.
 *
 * @param      loader_menu  menu instance, may be NULL
 */
void loader_menu_reload(LoaderMenu* loader_menu);

/** Read a FAP's manifest name and icon.
 *
 * On success @p name holds the manifest name and @p icon a heap-allocated Icon
 * owned by the caller, to be released with loader_menu_free_fap_icon().
 *
 * @param      storage  storage record
 * @param      path     path to the .fap
 * @param[out] name     manifest name
 * @param[out] icon     manifest icon, caller owns
 *
 * @return     true if the manifest was read
 */
bool loader_menu_load_fap_meta(
    Storage* storage,
    FuriString* path,
    FuriString* name,
    const Icon** icon);

/** Release an Icon returned by loader_menu_load_fap_meta().
 *
 * @param      icon  icon to free, may be NULL
 */
void loader_menu_free_fap_icon(const Icon* icon);

#ifdef __cplusplus
}
#endif
