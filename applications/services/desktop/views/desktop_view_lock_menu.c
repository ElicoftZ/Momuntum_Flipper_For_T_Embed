#include <furi.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <notification/notification_messages_notes.h>
#include <notification/notification_app.h>
#include <furi_hal_power.h>
#include <furi_hal_rtc.h>
#include <wifi/wlan_hal.h>
#include <stdio.h>

#include "../desktop_i.h"
#include "../helpers/qflipper_bridge.h"
#include "desktop_view_lock_menu.h"

/* Two selectable Control Centers: T-Embed restores the exact OFW scrolling
 * menu saved on August 28 before the Momentum port, while Momentum keeps its
 * exact fixed eight-tile composition. */

typedef enum {
    CcBluetooth,
    CcWifi,
    CcDarkMode,
    CcWake,
    CcPcLink,
    CcLock,
    CcSettings,
    CcUsbStorage,
    CcWebFs,
    CcMesh,
    CcBattery,
    CcDualBoot,
    CcBrightness,
    CcVolume,
    CcIdCount,
} CcId;

/* Exact control order and geometry from official Momentum's Control Center.
 * The input policy is adapted for the T-Embed wheel, but this fixed set must
 * stay at eight controls so the visual mode remains Momentum-compatible. */
typedef enum {
    MomentumCcWake,
    MomentumCcSettings,
    MomentumCcQFlipper,
    MomentumCcDualBoot,
    MomentumCcBluetooth,
    MomentumCcMomentum,
    MomentumCcBrightness,
    MomentumCcVolume,
    MomentumCcCount,
} MomentumCcId;

typedef enum {
    CcKindToggle,
    CcKindAction,
    CcKindSlider,
} CcKind;

typedef struct {
    CcKind kind;
    const char* label;
    const Icon* icon;
    DesktopEvent event;
} CcInfo;

static const CcInfo cc_info[CcIdCount] = {
    [CcBluetooth] =
        {CcKindToggle, "BT", &I_CC_Bluetooth_16x16, DesktopLockMenuEventBluetoothToggle},
    [CcWifi] = {CcKindToggle, "WiFi", &I_CC_Wifi_16x16, DesktopLockMenuEventWifiToggle},
    [CcDarkMode] = {CcKindToggle, "Dark", &I_CC_DarkMode_16x16, DesktopLockMenuEventDarkModeToggle},
    [CcWake] = {CcKindToggle, "Wake", &I_CC_Wake_16x16, DesktopLockMenuEventWakeToggle},
    [CcPcLink] =
        {CcKindToggle, "PC Link", &I_CC_Qflipper_16x16, DesktopLockMenuEventPcLinkToggle},
    [CcLock] = {CcKindAction, "Lock", &I_CC_Lock_16x16, DesktopLockMenuEventLock},
    [CcSettings] = {CcKindAction, "Set", &I_CC_Settings_16x16, DesktopLockMenuEventOpenSettings},
    [CcUsbStorage] =
        {CcKindAction, "USB", &I_CC_UsbStorage_16x16, DesktopLockMenuEventUsbStorage},
    [CcMesh] = {CcKindAction, "Mesh", &I_CC_Mesh_16x16, DesktopLockMenuEventMeshClients},
    [CcWebFs] = {CcKindAction, "Files", &I_CC_Wifi_16x16, DesktopLockMenuEventWebFs},
    [CcBattery] =
        {CcKindAction, "Battery", &I_Battery_16x16, DesktopLockMenuEventOpenPower},
    [CcDualBoot] =
        {CcKindAction, "Boot", &I_CC_DualBoot_16x16, DesktopLockMenuEventDualBoot},
    [CcBrightness] = {CcKindSlider, "Bright", &I_CC_Brightness_16x16, 0},
    [CcVolume] = {CcKindSlider, "Volume", &I_CC_Volume_16x16, 0},
};

#define OFW_MENU_VISIBLE 3
#define CC_BRIGHTNESS_MIN 0.20f

/* Exact August 28 pre-Momentum T-Embed row set, built from availability. */
static CcId s_items[CcIdCount];
static uint8_t s_count = 0;

/* Draw needs the notification service for the slider values; there is only ever
 * one lock-menu instance, so a file static mirrors the struct field. */
static NotificationApp* s_notification = NULL;

/* Short preview used after each volume step. */
static const NotificationSequence cc_sequence_volume_preview = {
    &message_note_c5,
    &message_delay_100,
    &message_sound_off,
    NULL,
};

/* Transient overlay message (WiFi start failure, Dual Boot with no slot, ...). */
#define LOCK_MENU_MESSAGE_LEN 40
static char s_message[LOCK_MENU_MESSAGE_LEN];
static char s_hint[LOCK_MENU_MESSAGE_LEN];
static bool s_has_message = false;

/* Used while formatting the OFW list's live Brightness/Volume labels. */
static float cc_slider_value(CcId id);

static void cc_build_items(bool usb_available, bool dualboot_available) {
    s_count = 0;
    if(usb_available) {
        s_items[s_count++] = CcPcLink;
        s_items[s_count++] = CcUsbStorage;
    }
    s_items[s_count++] = CcBluetooth;
    s_items[s_count++] = CcWifi;
    s_items[s_count++] = CcWebFs;
    s_items[s_count++] = CcMesh;
    s_items[s_count++] = CcWake;
    if(dualboot_available) s_items[s_count++] = CcDualBoot;
}

static void cc_scroll_to(DesktopLockMenuViewModel* model) {
    if(model->idx < model->top_row) {
        model->top_row = model->idx;
    } else if(model->idx >= model->top_row + OFW_MENU_VISIBLE) {
        model->top_row = model->idx - OFW_MENU_VISIBLE + 1;
    }
}

static void cc_ofw_label(
    char* text,
    size_t text_size,
    const DesktopLockMenuViewModel* model,
    CcId id) {
    const bool adjusting =
        model->adjust && model->idx < s_count && s_items[model->idx] == id;
    switch(id) {
    case CcBluetooth:
        strlcpy(
            text, model->bt_on ? "Disable Bluetooth" : "Enable Bluetooth", text_size);
        break;
    case CcWifi:
        strlcpy(text, model->wifi_on ? "Disable WiFi" : "Enable WiFi", text_size);
        break;
    case CcDarkMode:
        snprintf(text, text_size, "Dark Mode: %s", model->dark_on ? "ON" : "OFF");
        break;
    case CcWake:
        snprintf(text, text_size, "Wake: %s", model->wake_on ? "ON" : "OFF");
        break;
    case CcPcLink:
        strlcpy(
            text, model->qflipper_on ? "Disable qFlipper" : "Enable qFlipper", text_size);
        break;
    case CcLock:
        strlcpy(text, "Lock", text_size);
        break;
    case CcSettings:
        strlcpy(text, "Settings", text_size);
        break;
    case CcUsbStorage:
        strlcpy(text, "USB-Storage", text_size);
        break;
    case CcMesh:
        strlcpy(text, "Mesh Clients", text_size);
        break;
    case CcWebFs:
        strlcpy(text, "Web-Filesystem", text_size);
        break;
    case CcBattery:
        snprintf(text, text_size, "Battery: %u%%", (unsigned)furi_hal_power_get_pct());
        break;
    case CcDualBoot:
        strlcpy(text, "Dual Boot", text_size);
        break;
    case CcBrightness:
        snprintf(
            text,
            text_size,
            "Bright: %u%%%s",
            (unsigned)(cc_slider_value(CcBrightness) * 100.0f + 0.5f),
            adjusting ? " *" : "");
        break;
    case CcVolume:
        snprintf(
            text,
            text_size,
            "Volume: %u%%%s",
            (unsigned)(cc_slider_value(CcVolume) * 100.0f + 0.5f),
            adjusting ? " *" : "");
        break;
    default:
        text[0] = '\0';
        break;
    }
}

/* Slider value in 0..1. Reads the live notification settings. */
static float cc_slider_value(CcId id) {
    if(!s_notification) return 0.0f;
    if(id == CcBrightness) return s_notification->settings.display_brightness;
    if(id == CcVolume) return s_notification->settings.speaker_volume;
    return 0.0f;
}

static void cc_slider_adjust(DesktopLockMenuView* lock_menu, CcId id, float delta) {
    NotificationApp* n = lock_menu->notification;
    if(!n) return;
    float* v = NULL;
    if(id == CcBrightness) {
        v = &n->settings.display_brightness;
    } else if(id == CcVolume) {
        v = &n->settings.speaker_volume;
    }
    if(!v) return;

    float nv = *v + delta;
    const float minimum = id == CcBrightness ? CC_BRIGHTNESS_MIN : 0.0f;
    if(nv < minimum) nv = minimum;
    if(nv > 1.0f) nv = 1.0f;
    *v = nv;

    /* Apply/preview every step immediately, matching Sound & Display settings. */
    if(id == CcBrightness) {
        notification_message(n, &sequence_display_backlight_force_on);
    } else if(id == CcVolume) {
        notification_message(n, &cc_sequence_volume_preview);
    }
}

void desktop_lock_menu_set_callback(
    DesktopLockMenuView* lock_menu,
    DesktopLockMenuViewCallback callback,
    void* context) {
    if(!lock_menu || !callback) return;
    lock_menu->callback = callback;
    lock_menu->context = context;
}

void desktop_lock_menu_set_notification(
    DesktopLockMenuView* lock_menu,
    NotificationApp* notification) {
    if(!lock_menu) return;
    lock_menu->notification = notification;
    s_notification = notification;

    /* A zero backlight makes a wheel-only device effectively impossible to
     * recover. Match Sound & Display's 20% floor and repair older saved values
     * as soon as the Control Centre opens. */
    if(notification && notification->settings.display_brightness < CC_BRIGHTNESS_MIN) {
        notification->settings.display_brightness = CC_BRIGHTNESS_MIN;
        notification_message(notification, &sequence_display_backlight_force_on);
        notification_message_save_settings(notification);
    }
}

void desktop_lock_menu_set_style(
    DesktopLockMenuView* lock_menu,
    DesktopControlCenterStyle style) {
    if(!lock_menu) return;
    if(style >= DesktopControlCenterStyleCount) style = DesktopControlCenterStyleCurrent;

    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            if(model->style != style) {
                model->style = style;
                model->idx = 0;
                model->top_row = 0;
                model->adjust = false;
            }
        },
        true);
}

void desktop_lock_menu_show_message(
    DesktopLockMenuView* lock_menu,
    const char* text,
    const char* hint) {
    if(!lock_menu || !text) return;

    strlcpy(s_message, text, LOCK_MENU_MESSAGE_LEN);
    strlcpy(s_hint, hint ? hint : "", LOCK_MENU_MESSAGE_LEN);
    s_has_message = true;

    with_view_model(lock_menu->view, DesktopLockMenuViewModel * model, { UNUSED(model); }, true);
}

void desktop_lock_menu_set_idx(DesktopLockMenuView* lock_menu, uint8_t idx) {
    if(!lock_menu) return;
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            const uint8_t count = model->style == DesktopControlCenterStyleMomentum ?
                                      MomentumCcCount :
                                      s_count;
            if(count && idx >= count) idx = count - 1;
            model->idx = idx;
            if(model->style == DesktopControlCenterStyleCurrent) cc_scroll_to(model);
        },
        true);
}

void desktop_lock_menu_set_states(
    DesktopLockMenuView* lock_menu,
    bool usb_available,
    bool qflipper_on,
    bool bt_on,
    bool wifi_on,
    bool wake_on,
    bool dark_on,
    bool dualboot_available) {
    if(!lock_menu) return;
    cc_build_items(usb_available, dualboot_available);
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            model->bt_on = bt_on;
            model->wifi_on = wifi_on;
            model->wake_on = wake_on;
            model->dark_on = dark_on;
            model->qflipper_on = qflipper_on;
            /* Keep the selection valid across a rebuild (a toggle can add or
             * drop a tile). */
            const uint8_t count = model->style == DesktopControlCenterStyleMomentum ?
                                      MomentumCcCount :
                                      s_count;
            if(count && model->idx >= count) model->idx = count - 1;
            if(model->style == DesktopControlCenterStyleCurrent) cc_scroll_to(model);
        },
        true);
}

void desktop_lock_menu_refresh_live(DesktopLockMenuView* lock_menu) {
    if(!lock_menu) return;
    with_view_model(lock_menu->view, DesktopLockMenuViewModel * model, { UNUSED(model); }, true);
}

static void cc_draw_ofw(Canvas* canvas, const DesktopLockMenuViewModel* model) {
    /* OFW/T-Embed door composition: three text rows with a scrolling frame. */
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_icon(canvas, -57, STATUS_BAR_Y_SHIFT, &I_DoorLeft_70x55);
    canvas_draw_icon(canvas, 116, STATUS_BAR_Y_SHIFT, &I_DoorRight_70x55);
    canvas_set_font(canvas, FontSecondary);

    for(uint8_t row = 0; row < OFW_MENU_VISIBLE; ++row) {
        const uint8_t index = model->top_row + row;
        if(index >= s_count) break;

        char label[32];
        cc_ofw_label(label, sizeof(label), model, s_items[index]);

        canvas_draw_str_aligned(
            canvas,
            64,
            9 + (row * 17) + STATUS_BAR_Y_SHIFT,
            AlignCenter,
            AlignCenter,
            label);

        if(model->idx == index) {
            elements_frame(canvas, 15, 1 + (row * 17) + STATUS_BAR_Y_SHIFT, 98, 15);
        }
    }
}

static void cc_draw_momentum(Canvas* canvas, const DesktopLockMenuViewModel* m) {
    /* This is the official Momentum 3x2 + two vertical-slider composition. */
    const int8_t slider_total = 58;
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontBatteryPercent);

    for(uint8_t i = 0; i < MomentumCcCount; ++i) {
        const bool selected = m->idx == i;
        const bool toggle = i < MomentumCcBrightness;
        int8_t x;
        int8_t y;
        int8_t w;
        int8_t h;
        bool enabled = false;
        uint8_t value = 0;
        const Icon* icon = NULL;

        if(toggle) {
            x = 2 + 32 * (i / 2);
            y = 2 + 32 * (i % 2);
            w = 28;
            h = 28;
        } else {
            x = 98 + 16 * (i % 2);
            y = 2;
            w = 12;
            h = 60;
        }

        switch(i) {
        case MomentumCcWake:
            icon = &I_CC_Wake_16x16;
            enabled = m->wake_on;
            break;
        case MomentumCcSettings:
            icon = &I_CC_Settings_16x16;
            break;
        case MomentumCcQFlipper:
            /* QFlipper hand/device mark, toggled when PC Link owns USB. */
            icon = m->usb_storage_mode ? &I_CC_UsbStorage_16x16 : &I_CC_Qflipper_16x16;
            enabled = !m->usb_storage_mode && m->qflipper_on;
            break;
        case MomentumCcDualBoot:
            /* Two overlapping tabs: protected Flipper + replaceable slot. */
            icon = &I_CC_DualBoot_16x16;
            break;
        case MomentumCcBluetooth:
            /* Short OK toggles the visible radio; long OK switches between
             * the mutually exclusive WiFi and BLE modes. */
            icon = m->wifi_on ? &I_CC_Wifi_16x16 : &I_CC_Bluetooth_16x16;
            enabled = m->bt_on || m->wifi_on;
            break;
        case MomentumCcMomentum:
            icon = &I_CC_Momentum_16x16;
            break;
        case MomentumCcBrightness:
            icon = &I_Pin_star_7x7;
            value = slider_total - (uint8_t)(cc_slider_value(CcBrightness) * slider_total);
            break;
        case MomentumCcVolume:
            icon = furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode) ? &I_Muted_8x8 :
                                                                        &I_Volup_8x6;
            value = slider_total - (uint8_t)(cc_slider_value(CcVolume) * slider_total);
            break;
        default:
            break;
        }

        if(selected) {
            elements_bold_rounded_frame(canvas, x - 1, y - 1, w + 1, h + 1);
        } else {
            canvas_draw_rframe(canvas, x, y, w, h, 5);
        }

        if(toggle) {
            if(enabled) {
                canvas_draw_rbox(canvas, x, y, w, h, 5);
                canvas_set_color(canvas, ColorWhite);
            }
            canvas_draw_icon(
                canvas,
                x + (w - icon_get_width(icon)) / 2,
                y + (h - icon_get_height(icon)) / 2,
                icon);
            if(enabled) canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_icon(
                canvas,
                x + (w - icon_get_width(icon)) / 2,
                y + (h - icon_get_height(icon)) / 2,
                icon);
            canvas_set_color(canvas, ColorXOR);
            canvas_draw_box(canvas, x + 1, y + 1 + value, w - 2, h - 2 - value);
            canvas_set_color(canvas, selected ? ColorBlack : ColorWhite);
            canvas_draw_dot(canvas, x + 1, y + 1);
            canvas_draw_dot(canvas, x + 1, y + h - 2);
            canvas_draw_dot(canvas, x + w - 2, y + 1);
            canvas_draw_dot(canvas, x + w - 2, y + h - 2);
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rframe(canvas, x, y, w, h, 5);

            if(selected && m->adjust) {
                /* Wheel-only affordance: upstream uses directional buttons;
                 * the inner line marks that rotation now adjusts the slider. */
                canvas_draw_rframe(canvas, x + 2, y + 2, w - 4, h - 4, 3);
            }
        }
    }
}

void desktop_lock_menu_draw_callback(Canvas* canvas, void* model) {
    DesktopLockMenuViewModel* m = model;

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    if(m->style == DesktopControlCenterStyleMomentum) {
        cc_draw_momentum(canvas, m);
    } else {
        cc_draw_ofw(canvas, m);
    }

    if(s_has_message) {
        const bool have_hint = s_hint[0] != '\0';
        const uint8_t h = have_hint ? 42 : 32;
        const uint8_t y = 10;

        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 6, y, 116, h);
        canvas_set_color(canvas, ColorBlack);
        elements_frame(canvas, 6, y, 116, h);

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, y + 11, AlignCenter, AlignCenter, s_message);
        if(have_hint) {
            canvas_draw_str_aligned(canvas, 64, y + 22, AlignCenter, AlignCenter, s_hint);
        }
        canvas_draw_str_aligned(canvas, 64, y + h - 9, AlignCenter, AlignCenter, "Press any key");
    }
}

View* desktop_lock_menu_get_view(DesktopLockMenuView* lock_menu) {
    furi_check(lock_menu);
    return lock_menu->view;
}

static DesktopEvent cc_momentum_event(MomentumCcId id) {
    switch(id) {
    case MomentumCcWake:
        return DesktopLockMenuEventWakeToggle;
    case MomentumCcSettings:
        return DesktopLockMenuEventOpenSettings;
    case MomentumCcQFlipper:
        return DesktopLockMenuEventPcLinkToggle;
    case MomentumCcDualBoot:
        return DesktopLockMenuEventDualBoot;
    case MomentumCcBluetooth:
        return DesktopLockMenuEventBluetoothToggle;
    case MomentumCcMomentum:
        return DesktopLockMenuEventOpenMomentum;
    default:
        return 0;
    }
}

static bool cc_momentum_input(DesktopLockMenuView* lock_menu, InputEvent* event) {
    const bool nav = event->type == InputTypeShort || event->type == InputTypeRepeat;
    bool consumed = false;
    uint8_t idx = 0;
    bool in_adjust = false;
    bool wifi_on = false;
    bool usb_storage_mode = false;

    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            if(model->idx >= MomentumCcCount) model->idx = MomentumCcCount - 1;
            if(nav && (event->key == InputKeyUp || event->key == InputKeyDown)) {
                if(model->adjust) {
                    cc_slider_adjust(
                        lock_menu,
                        model->idx == MomentumCcBrightness ? CcBrightness : CcVolume,
                        event->key == InputKeyUp ? 0.05f : -0.05f);
                } else if(event->key == InputKeyUp) {
                    model->idx = model->idx == 0 ? MomentumCcCount - 1 : model->idx - 1;
                } else {
                    model->idx = model->idx + 1 >= MomentumCcCount ? 0 : model->idx + 1;
                }
                consumed = true;
            }
            idx = model->idx;
            in_adjust = model->adjust;
            wifi_on = model->wifi_on;
            usb_storage_mode = model->usb_storage_mode;
        },
        consumed);

    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        if(idx >= MomentumCcBrightness) {
            bool leaving = false;
            with_view_model(
                lock_menu->view,
                DesktopLockMenuViewModel * model,
                {
                    model->adjust = !model->adjust;
                    leaving = !model->adjust;
                },
                true);
            if(leaving && lock_menu->notification) {
                notification_message_save_settings(lock_menu->notification);
            }
        } else {
            const DesktopEvent desktop_event = idx == MomentumCcQFlipper && usb_storage_mode ?
                                                   DesktopLockMenuEventUsbStorage :
                                               idx == MomentumCcBluetooth && wifi_on ?
                                                   DesktopLockMenuEventWifiToggle :
                                                   cc_momentum_event((MomentumCcId)idx);
            if(desktop_event) lock_menu->callback(desktop_event, lock_menu->context);
        }
        consumed = true;
    } else if(event->key == InputKeyOk && event->type == InputTypeLong) {
        if(idx == MomentumCcQFlipper) {
            with_view_model(
                lock_menu->view,
                DesktopLockMenuViewModel * model,
                { model->usb_storage_mode = !model->usb_storage_mode; },
                true);
            consumed = true;
        } else if(idx == MomentumCcBluetooth) {
            /* Hold switches between the exclusive WiFi/BLE modes. */
            lock_menu->callback(DesktopLockMenuEventRadioModeToggle, lock_menu->context);
            consumed = true;
        } else if(idx == MomentumCcBrightness) {
            /* Preserve the two upstream slider-click actions without
             * sacrificing short-OK wheel adjustment. */
            lock_menu->callback(DesktopLockMenuEventOpenDisplay, lock_menu->context);
            consumed = true;
        } else if(idx == MomentumCcVolume) {
            lock_menu->callback(DesktopLockMenuEventStealthToggle, lock_menu->context);
            consumed = true;
        }
    } else if(event->key == InputKeyBack && event->type == InputTypeShort && in_adjust) {
        with_view_model(
            lock_menu->view, DesktopLockMenuViewModel * model, { model->adjust = false; }, true);
        if(lock_menu->notification) notification_message_save_settings(lock_menu->notification);
        consumed = true;
    }

    return consumed;
}

bool desktop_lock_menu_input_callback(InputEvent* event, void* context) {
    if(!event || !context) return false;

    DesktopLockMenuView* lock_menu = context;
    bool consumed = false;

    /* The overlay swallows the dismissing press so it does not also actuate a
     * tile underneath. */
    if(s_has_message) {
        if(event->type == InputTypeShort || event->type == InputTypeLong) {
            s_has_message = false;
            with_view_model(
                lock_menu->view, DesktopLockMenuViewModel * model, { UNUSED(model); }, true);
        }
        return true;
    }

    DesktopControlCenterStyle style = DesktopControlCenterStyleCurrent;
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        { style = model->style; },
        false);
    if(style == DesktopControlCenterStyleMomentum) {
        return cc_momentum_input(lock_menu, event);
    }

    if(s_count == 0) return false;

    const bool nav = (event->type == InputTypeShort) || (event->type == InputTypeRepeat);

    CcId selected = CcBluetooth;
    bool in_adjust = false;
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            if(model->idx >= s_count) model->idx = s_count - 1;

            if(nav && (event->key == InputKeyUp || event->key == InputKeyDown)) {
                if(model->adjust) {
                    cc_slider_adjust(
                        lock_menu,
                        s_items[model->idx],
                        event->key == InputKeyUp ? 0.05f : -0.05f);
                } else if(event->key == InputKeyUp) {
                    model->idx = (model->idx == 0) ? (s_count - 1) : (model->idx - 1);
                } else {
                    model->idx = (model->idx + 1 >= s_count) ? 0 : (model->idx + 1);
                }
                cc_scroll_to(model);
                consumed = true;
            }
            selected = s_items[model->idx];
            in_adjust = model->adjust;
        },
        consumed);

    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        if(cc_info[selected].kind == CcKindSlider) {
            bool leaving = false;
            with_view_model(
                lock_menu->view,
                DesktopLockMenuViewModel * model,
                {
                    model->adjust = !model->adjust;
                    leaving = !model->adjust;
                },
                true);
            if(leaving && lock_menu->notification) {
                notification_message_save_settings(lock_menu->notification);
            }
        } else if(cc_info[selected].event) {
            lock_menu->callback(cc_info[selected].event, lock_menu->context);
        }
        consumed = true;
    } else if(event->key == InputKeyBack && event->type == InputTypeShort && in_adjust) {
        with_view_model(
            lock_menu->view, DesktopLockMenuViewModel * model, { model->adjust = false; }, true);
        if(lock_menu->notification) notification_message_save_settings(lock_menu->notification);
        consumed = true;
    }

    return consumed;
}

DesktopLockMenuView* desktop_lock_menu_alloc(void) {
    DesktopLockMenuView* lock_menu = malloc(sizeof(DesktopLockMenuView));
    lock_menu->callback = NULL;
    lock_menu->context = NULL;
    lock_menu->notification = NULL;
    lock_menu->view = view_alloc();
    view_allocate_model(lock_menu->view, ViewModelTypeLocking, sizeof(DesktopLockMenuViewModel));
    view_set_context(lock_menu->view, lock_menu);
    view_set_draw_callback(lock_menu->view, (ViewDrawCallback)desktop_lock_menu_draw_callback);
    view_set_input_callback(lock_menu->view, desktop_lock_menu_input_callback);

    /* Default list until the scene fills in real states on enter. */
    cc_build_items(false, false);
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            model->style = DesktopControlCenterStyleCurrent;
            model->usb_storage_mode = false;
        },
        false);

    return lock_menu;
}

void desktop_lock_menu_free(DesktopLockMenuView* lock_menu_view) {
    furi_check(lock_menu_view);
    view_free(lock_menu_view->view);
    free(lock_menu_view);
}
