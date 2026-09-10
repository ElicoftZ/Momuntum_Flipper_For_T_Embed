#pragma once

#include <gui/view.h>
#include "desktop_events.h"
#include "../desktop_settings.h"

#define HINT_TIMEOUT 2

typedef struct NotificationApp NotificationApp;

typedef struct DesktopLockMenuView DesktopLockMenuView;

typedef void (*DesktopLockMenuViewCallback)(DesktopEvent event, void* context);

struct DesktopLockMenuView {
    View* view;
    DesktopLockMenuViewCallback callback;
    void* context;
    /* Brightness/Volume are adjusted live from inside the view, mirroring
     * Momentum's Control Centre. NULL is tolerated (sliders read as empty). */
    NotificationApp* notification;
};

/* Shared Control Centre model: T-Embed uses an OFW scrolling list and
 * Momentum uses its fixed tile grid. Both are adapted to the Up/Down wheel. */
typedef struct {
    uint8_t idx; // selected item
    uint8_t top_row; // first visible T-Embed list item
    bool adjust; // slider adjust sub-mode
    // Toggle states, filled by desktop_lock_menu_set_states():
    bool bt_on;
    bool wifi_on;
    bool wake_on;
    bool dark_on;
    bool qflipper_on;
    bool usb_storage_mode; // Hold the qFlipper tile to select USB storage.
    DesktopControlCenterStyle style;
} DesktopLockMenuViewModel;

void desktop_lock_menu_set_callback(
    DesktopLockMenuView* lock_menu,
    DesktopLockMenuViewCallback callback,
    void* context);

View* desktop_lock_menu_get_view(DesktopLockMenuView* lock_menu);
void desktop_lock_menu_set_idx(DesktopLockMenuView* lock_menu, uint8_t idx);

/** Choose between the full T-Embed OFW list and Momentum's exact fixed
 *  eight-control tile layout. */
void desktop_lock_menu_set_style(
    DesktopLockMenuView* lock_menu,
    DesktopControlCenterStyle style);

/** Give the view the notification service so the Brightness/Volume controls can
 *  read and adjust the live values. Safe to pass NULL. */
void desktop_lock_menu_set_notification(
    DesktopLockMenuView* lock_menu,
    NotificationApp* notification);

/** Draw a short message over the menu until the next key press. Used for
 *  failures that would otherwise be invisible (e.g. Dual Boot with an empty
 *  ota_0 slot). Pass NULL hint for a message that needs no follow-up. */
void desktop_lock_menu_show_message(
    DesktopLockMenuView* lock_menu,
    const char* text,
    const char* hint);

/** Rebuild the restored pre-Momentum T-Embed OFW list and refresh toggle states.
 *  `usb_available` gates qFlipper / USB-Storage (USB-OTG is ESP32-S3/S2 only),
 *  `dualboot_available` gates the Dual Boot entry. The remaining flags are the
 *  current on/off states drawn in either layout. */
void desktop_lock_menu_set_states(
    DesktopLockMenuView* lock_menu,
    bool usb_available,
    bool qflipper_on,
    bool bt_on,
    bool wifi_on,
    bool wake_on,
    bool dark_on,
    bool dualboot_available);

/** Redraw live status values (WiFi connection and battery) without rebuilding
 *  the tile list or disturbing wheel/slider state. */
void desktop_lock_menu_refresh_live(DesktopLockMenuView* lock_menu);

DesktopLockMenuView* desktop_lock_menu_alloc(void);
void desktop_lock_menu_free(DesktopLockMenuView* lock_menu);
