#include <furi.h>
#include <gui/elements.h>
#include <assets_icons.h>

#include "../desktop_i.h"
#include "desktop_view_lock_menu.h"

// Headroom over the five rows built today; lock_menu_build_items() does not
// bounds-check, so this must stay ahead of the row count.
#define LOCK_MENU_MAX_ITEMS 8

// Menu items and events are built dynamically from the current toggle states:
//   qFlipper       Enable/Disable (background RPC bridge)   [USB-OTG only]
//   USB-Storage    open the full-screen mass-storage scene  [USB-OTG only]
//   Bluetooth      Enable/Disable
//   Mesh: Off/Master/Client    cycle the mesh role
//   Mesh Clients   open the discovery/pairing scene          [Master only]
//   Wake           hold the backlight on, suppress deep sleep

typedef struct {
    const char* label;
    DesktopEvent event;
} LockMenuItem;

static LockMenuItem s_items[LOCK_MENU_MAX_ITEMS];
static uint8_t s_item_count = 0;

// Rows that fit on screen below the status bar (each item is 17px tall).
#define LOCK_MENU_VISIBLE 3
static uint8_t s_top = 0; // index of the first visible item

/* Transient message drawn over the menu and cleared by any key, for actions
 * that would otherwise fail silently. A Popup would need its own scene and
 * DialogEx needs Left/Right, which this board cannot produce -- but this view
 * already owns its drawing. */
#define LOCK_MENU_MESSAGE_LEN 40
static char s_message[LOCK_MENU_MESSAGE_LEN];
static char s_hint[LOCK_MENU_MESSAGE_LEN];
static bool s_has_message = false;

// Keep the selected item inside the visible window.
static void lock_menu_scroll_to(uint8_t idx) {
    if(idx < s_top) {
        s_top = idx;
    } else if(idx >= s_top + LOCK_MENU_VISIBLE) {
        s_top = idx - LOCK_MENU_VISIBLE + 1;
    }
}

static void lock_menu_build_items(
    bool usb_available,
    bool qflipper_on,
    bool bt_on,
    bool wake_on) {
    s_item_count = 0;

    if(usb_available) {
        s_items[s_item_count++] = (LockMenuItem){
            qflipper_on ? "Disable qFlipper" : "Enable qFlipper",
            DesktopLockMenuEventQflipperToggle};
        s_items[s_item_count++] = (LockMenuItem){"USB-Storage", DesktopLockMenuEventUsbStorage};
    }

    s_items[s_item_count++] = (LockMenuItem){
        bt_on ? "Disable Bluetooth" : "Enable Bluetooth", DesktopLockMenuEventBluetoothToggle};

    /* Mesh: der T-Embed ist immer Master — kein Mode-Toggle, "Mesh Clients"
     * (Discovery/Pair) ist immer verfügbar. */
    s_items[s_item_count++] = (LockMenuItem){"Mesh Clients", DesktopLockMenuEventMeshClients};

    /* Wake mode holds the backlight on and stops the idle dim and deep sleep. */
    s_items[s_item_count++] = (LockMenuItem){
        wake_on ? "Wake: ON" : "Wake: OFF", DesktopLockMenuEventWakeToggle};
}

void desktop_lock_menu_set_callback(
    DesktopLockMenuView* lock_menu,
    DesktopLockMenuViewCallback callback,
    void* context) {
    furi_assert(lock_menu);
    furi_assert(callback);
    lock_menu->callback = callback;
    lock_menu->context = context;
}

void desktop_lock_menu_show_message(
    DesktopLockMenuView* lock_menu,
    const char* text,
    const char* hint) {
    furi_assert(lock_menu);
    furi_assert(text);

    strlcpy(s_message, text, LOCK_MENU_MESSAGE_LEN);
    strlcpy(s_hint, hint ? hint : "", LOCK_MENU_MESSAGE_LEN);
    s_has_message = true;

    /* Force a redraw; the model is unchanged but the overlay is not part of it. */
    with_view_model(lock_menu->view, DesktopLockMenuViewModel * model, { UNUSED(model); }, true);
}

void desktop_lock_menu_set_idx(DesktopLockMenuView* lock_menu, uint8_t idx) {
    furi_assert(idx < LOCK_MENU_MAX_ITEMS);
    with_view_model(
        lock_menu->view, DesktopLockMenuViewModel * model, { model->idx = idx; }, true);
}

void desktop_lock_menu_set_states(
    DesktopLockMenuView* lock_menu,
    bool usb_available,
    bool qflipper_on,
    bool bt_on,
    bool wake_on) {
    lock_menu_build_items(usb_available, qflipper_on, bt_on, wake_on);
    /* Index nicht resetten — Caller (refresh nach Toggle) erwartet, dass die
     * Selektion stehen bleibt; bei out-of-range clampen wir, damit der Wechsel
     * vom Master- in den Off-Modus (verliert "Mesh Clients") nicht ins Leere
     * zeigt. */
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            if(model->idx >= s_item_count) model->idx = s_item_count - 1;
            lock_menu_scroll_to(model->idx);
        },
        true);
}

void desktop_lock_menu_draw_callback(Canvas* canvas, void* model) {
    DesktopLockMenuViewModel* m = model;

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_icon(canvas, -57, 0 + STATUS_BAR_Y_SHIFT, &I_DoorLeft_70x55);
    canvas_draw_icon(canvas, 116, 0 + STATUS_BAR_Y_SHIFT, &I_DoorRight_70x55);

    canvas_set_font(canvas, FontSecondary);
    for(uint8_t row = 0; row < LOCK_MENU_VISIBLE; ++row) {
        uint8_t i = s_top + row;
        if(i >= s_item_count) break;

        canvas_draw_str_aligned(
            canvas,
            64,
            9 + (row * 17) + STATUS_BAR_Y_SHIFT,
            AlignCenter,
            AlignCenter,
            s_items[i].label);

        if(m->idx == i) {
            elements_frame(canvas, 15, 1 + (row * 17) + STATUS_BAR_Y_SHIFT, 98, 15);
        }
    }

    if(s_has_message) {
        const bool have_hint = s_hint[0] != '\0';
        const uint8_t h = have_hint ? 42 : 32;
        const uint8_t y = 10 + STATUS_BAR_Y_SHIFT;

        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 6, y, 116, h);
        canvas_set_color(canvas, ColorBlack);
        elements_frame(canvas, 6, y, 116, h);

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, y + 11, AlignCenter, AlignCenter, s_message);
        if(have_hint) {
            canvas_draw_str_aligned(canvas, 64, y + 22, AlignCenter, AlignCenter, s_hint);
        }
        canvas_draw_str_aligned(
            canvas, 64, y + h - 9, AlignCenter, AlignCenter, "Press any key");
    }
}

View* desktop_lock_menu_get_view(DesktopLockMenuView* lock_menu) {
    furi_assert(lock_menu);
    return lock_menu->view;
}

bool desktop_lock_menu_input_callback(InputEvent* event, void* context) {
    furi_assert(event);
    furi_assert(context);

    DesktopLockMenuView* lock_menu = context;
    uint8_t idx = 0;
    bool consumed = false;
    bool update = false;

    /* While the overlay is up it swallows the whole press, so the key that
     * dismisses it cannot also actuate the row underneath. */
    if(s_has_message) {
        if(event->type == InputTypeShort || event->type == InputTypeLong) {
            s_has_message = false;
            with_view_model(
                lock_menu->view, DesktopLockMenuViewModel * model, { UNUSED(model); }, true);
        }
        return true;
    }

    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            if((event->type == InputTypeShort) || (event->type == InputTypeRepeat)) {
                if(event->key == InputKeyUp) {
                    if(model->idx == 0) {
                        model->idx = s_item_count - 1;
                    } else {
                        model->idx--;
                    }
                    update = true;
                    consumed = true;
                } else if(event->key == InputKeyDown) {
                    if(model->idx >= s_item_count - 1) {
                        model->idx = 0;
                    } else {
                        model->idx++;
                    }
                    update = true;
                    consumed = true;
                }
                if(update) lock_menu_scroll_to(model->idx);
            }
            idx = model->idx;
        },
        update);

    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        if(idx < s_item_count) {
            lock_menu->callback(s_items[idx].event, lock_menu->context);
        }
        consumed = true;
    }

    return consumed;
}

DesktopLockMenuView* desktop_lock_menu_alloc(void) {
    DesktopLockMenuView* lock_menu = malloc(sizeof(DesktopLockMenuView));
    lock_menu->view = view_alloc();
    view_allocate_model(lock_menu->view, ViewModelTypeLocking, sizeof(DesktopLockMenuViewModel));
    view_set_context(lock_menu->view, lock_menu);
    view_set_draw_callback(lock_menu->view, (ViewDrawCallback)desktop_lock_menu_draw_callback);
    view_set_input_callback(lock_menu->view, desktop_lock_menu_input_callback);

    // Default until the scene fills in real states on enter.
    lock_menu_build_items(false, false, false, false);

    return lock_menu;
}

void desktop_lock_menu_free(DesktopLockMenuView* lock_menu_view) {
    furi_assert(lock_menu_view);

    view_free(lock_menu_view->view);
    free(lock_menu_view);
}
