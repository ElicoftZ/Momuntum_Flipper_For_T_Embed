#pragma once

#include <gui/view.h>
#include "desktop_events.h"

#define HINT_TIMEOUT 2

typedef struct DesktopLockMenuView DesktopLockMenuView;

typedef void (*DesktopLockMenuViewCallback)(DesktopEvent event, void* context);

struct DesktopLockMenuView {
    View* view;
    DesktopLockMenuViewCallback callback;
    void* context;
};

typedef struct {
    uint8_t idx;
} DesktopLockMenuViewModel;

void desktop_lock_menu_set_callback(
    DesktopLockMenuView* lock_menu,
    DesktopLockMenuViewCallback callback,
    void* context);

View* desktop_lock_menu_get_view(DesktopLockMenuView* lock_menu);
void desktop_lock_menu_set_idx(DesktopLockMenuView* lock_menu, uint8_t idx);

/** Draw a short message over the menu until the next key press.
 *
 * For failures that would otherwise be invisible -- pressing Dual Boot with an
 * empty ota_0 slot only logs and refreshes, which looks like nothing happened.
 *
 * @param hint optional second line telling the user what to do about it; pass
 *             NULL for a message that needs no follow-up.
 */
void desktop_lock_menu_show_message(
    DesktopLockMenuView* lock_menu,
    const char* text,
    const char* hint);

/** Rebuild the menu items from the current toggle states and reset the
 *  selection. `usb_available` gates the qFlipper / USB-Storage entries (USB-OTG
 *  is ESP32-S3/S2 only). "Mesh Clients" ist immer dabei (T-Embed ist immer
 *  Master). `dualboot_available` gates the second-firmware entry, so a
 *  single-app build shows no row that cannot work. */
void desktop_lock_menu_set_states(
    DesktopLockMenuView* lock_menu,
    bool usb_available,
    bool qflipper_on,
    bool bt_on,
    bool wake_on,
    bool dualboot_available);

DesktopLockMenuView* desktop_lock_menu_alloc(void);
void desktop_lock_menu_free(DesktopLockMenuView* lock_menu);
