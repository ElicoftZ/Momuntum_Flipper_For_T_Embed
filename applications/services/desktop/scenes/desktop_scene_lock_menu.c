#include <furi.h>
#include <gui/scene_manager.h>

#include <btshim.h>
#include <wlan_hal.h>
#include <wifi.h>

#include "../desktop_i.h"
#include "../views/desktop_view_lock_menu.h"
#include "../helpers/qflipper_bridge.h"

#include <notification/notification_messages.h>
#include <loader/loader.h>
#include <momentum/settings.h>
#include <furi_hal_power.h>
#include "furi_hal_usb_tinyusb_composite.h"
#include "desktop_scene.h"

#include "sdkconfig.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

/* qFlipper / USB-Storage need USB-OTG (ESP32-S3 / S2 only). */
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
#define LOCK_MENU_USB_AVAILABLE true
#else
#define LOCK_MENU_USB_AVAILABLE false
#endif

void desktop_scene_lock_menu_callback(DesktopEvent event, void* context) {
    Desktop* desktop = (Desktop*)context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

/* Wake mode starts off; the Control Centre tile owns exactly one insomnia
 * count and only acquires it when toggled on. */
static bool s_wake_mode = false;

/* USB enumeration takes time and the wheel can queue several short presses.
 * Ignore overlapping PC-Link actions so one physical press cannot start the
 * composite and immediately tear it back down. */
#define PC_LINK_ACTION_GUARD_MS 1500U
static uint32_t s_pc_link_last_action = 0;
static bool s_pc_link_action_seen = false;

static bool desktop_lock_menu_bt_enabled(void) {
    Bt* bt = furi_record_open(RECORD_BT);
    BtSettings settings;
    bt_get_settings(bt, &settings);
    furi_record_close(RECORD_BT);
    return settings.enabled;
}

static bool desktop_lock_menu_wifi_active(void) {
    return wlan_hal_is_user_enabled() || wlan_hal_is_boot_time_sync_active();
}

static bool desktop_lock_menu_bt_active(void) {
    return desktop_lock_menu_bt_enabled();
}

static bool desktop_lock_menu_apply_bt_enabled(bool enabled) {
    Bt* bt = furi_record_open(RECORD_BT);
    const bool result = bt_set_enabled(bt, enabled);
    furi_record_close(RECORD_BT);
    return result;
}

static bool desktop_lock_menu_set_bt_enabled(Desktop* desktop, bool enabled) {
    UNUSED(desktop);
    return desktop_lock_menu_apply_bt_enabled(enabled);
}

static bool desktop_lock_menu_set_wifi_enabled(Desktop* desktop, bool enabled) {
    if(!enabled) {
        wlan_hal_set_user_enabled(false);
        desktop_set_wifi_icon_state(desktop, false);
        return true;
    }

    if(wlan_hal_set_user_enabled(true)) {
        desktop_set_wifi_icon_state(desktop, true);
        return true;
    }

    desktop_set_wifi_icon_state(desktop, false);
    return false;
}

/* The Control Centre has one radio mode at a time. Release the active stack
 * first so its controller/host memory is available to the new stack. */
static bool desktop_lock_menu_switch_to_wifi(Desktop* desktop) {
    const bool bt_was_enabled = desktop_lock_menu_bt_enabled();
    if(bt_was_enabled && !desktop_lock_menu_set_bt_enabled(desktop, false)) return false;

    if(desktop_lock_menu_set_wifi_enabled(desktop, true)) return true;

    if(bt_was_enabled) desktop_lock_menu_set_bt_enabled(desktop, true);
    return false;
}

static bool desktop_lock_menu_switch_to_bt(Desktop* desktop) {
    const bool wifi_was_enabled = desktop_lock_menu_wifi_active();
    if(wifi_was_enabled && !desktop_lock_menu_set_wifi_enabled(desktop, false)) return false;

    if(desktop_lock_menu_set_bt_enabled(desktop, true)) return true;

    if(wifi_was_enabled) desktop_lock_menu_set_wifi_enabled(desktop, true);
    return false;
}

/* The second firmware's slot. NULL on a single-app build, which is what hides
 * the menu row. Never the running partition: this port lives in factory. */
static const esp_partition_t* desktop_lock_menu_dualboot_partition(void) {
#ifdef CONFIG_MOMENTUM_MULTIBOOT
    /* An empty pool is usable: opening the tile still shows the recovery
     * warning and the manager can install the first firmware. */
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "partpending");
#else
    const esp_partition_t* ota_0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);

    if(ota_0 && ota_0 == esp_ota_get_running_partition()) return NULL;
    return ota_0;
#endif
}

static bool desktop_lock_menu_wifi_enabled(void) {
    Wifi* wifi = furi_record_open(RECORD_WIFI);
    bool enabled = wifi_is_enabled(wifi);
    furi_record_close(RECORD_WIFI);
    return enabled;
}

/* Rebuild the menu from the live toggle states (used on enter and after a
 * toggle, so the Enable/Disable labels track reality). */
void desktop_scene_lock_menu_refresh(Desktop* desktop) {
    desktop_lock_menu_set_style(desktop->lock_menu, (DesktopControlCenterStyle)desktop->settings.control_center_style);
    desktop_lock_menu_set_states(
        desktop->lock_menu,
        LOCK_MENU_USB_AVAILABLE,
        qflipper_bridge_is_active(),
        desktop_lock_menu_bt_active(),
        desktop_lock_menu_wifi_active(),
        s_wake_mode,
        momentum_settings.dark_mode,
        desktop_lock_menu_dualboot_partition() != NULL);
}

void desktop_scene_lock_menu_on_enter(void* context) {
    Desktop* desktop = (Desktop*)context;

    /* Momentum owns the full canvas. T-Embed restores OFW's door-style list,
     * which is positioned below and used together with the normal status bar. */
    gui_set_status_bar_hidden(
        desktop->gui,
        desktop->settings.control_center_style == DesktopControlCenterStyleMomentum);
    desktop_lock_menu_set_callback(desktop->lock_menu, desktop_scene_lock_menu_callback, desktop);
    desktop_lock_menu_set_notification(desktop->lock_menu, desktop->notification);
    desktop_scene_lock_menu_refresh(desktop);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdLockMenu);
}

bool desktop_scene_lock_menu_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = (Desktop*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case DesktopLockMenuEventPcLinkToggle:
            {
                const uint32_t now = furi_get_tick();
                if(s_pc_link_action_seen &&
                   now - s_pc_link_last_action < furi_ms_to_ticks(PC_LINK_ACTION_GUARD_MS)) {
                    consumed = true;
                    break;
                }
                s_pc_link_action_seen = true;
                s_pc_link_last_action = now;
            }
            if(qflipper_bridge_pc_link_mode() == QflipperBridgePcLinkNativeSerial) {
                if(!qflipper_bridge_start()) {
                    desktop_lock_menu_show_message(
                        desktop->lock_menu, "PC Link failed", "Serial kept active");
                }
            } else {
                if(!qflipper_bridge_restore_native_serial()) {
                    desktop_lock_menu_show_message(
                        desktop->lock_menu, "USB recovery failed", "Try PC Link again");
                }
            }
            /* Stay in the menu; refresh Serial/qFlipper/USB-idle status. */
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventUsbStorage:
            /* The USB-Storage scene stops the qFlipper bridge itself (shared
             * composite / mutual exclusion). */
            scene_manager_next_scene(desktop->scene_manager, DesktopSceneUsbStorage);
            consumed = true;
            break;

        case DesktopLockMenuEventBluetoothToggle:
            if(desktop_lock_menu_bt_enabled()) {
                desktop_lock_menu_set_bt_enabled(desktop, false);
            } else if(!desktop_lock_menu_switch_to_bt(desktop)) {
                desktop_lock_menu_show_message(
                    desktop->lock_menu, "Bluetooth start failed", "WiFi was restored");
            }
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;
        case DesktopLockMenuEventWifiToggle:
            if(desktop_lock_menu_wifi_active()) {
                desktop_lock_menu_set_wifi_enabled(desktop, false);
            } else if(!desktop_lock_menu_switch_to_wifi(desktop)) {
                desktop_lock_menu_show_message(
                    desktop->lock_menu, "WiFi start failed", "Bluetooth was restored");
            }
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventRadioModeToggle:
            /* Holding the compact tile switches the exclusive radio mode. */
            if(desktop_lock_menu_wifi_active()) {
                if(!desktop_lock_menu_switch_to_bt(desktop)) {
                    desktop_lock_menu_show_message(
                        desktop->lock_menu, "Bluetooth start failed", "WiFi was restored");
                }
            } else if(!desktop_lock_menu_switch_to_wifi(desktop)) {
                desktop_lock_menu_show_message(
                    desktop->lock_menu, "WiFi start failed", "Bluetooth was restored");
            }
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventWakeToggle:
            s_wake_mode = !s_wake_mode;
            if(s_wake_mode) {
                furi_hal_power_insomnia_enter();
            } else {
                furi_hal_power_insomnia_exit();
            }
            notification_message(
                desktop->notification,
                s_wake_mode ? &sequence_display_backlight_enforce_on :
                              &sequence_display_backlight_enforce_auto);
            desktop_set_wake_icon_state(desktop, s_wake_mode);
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventDualBoot:
            /* The two-tab tile opens the manager. Booting directly here made
             * an empty/corrupt slot look like a dead button and skipped the
             * Recovery 2.0 compatibility checks. */
            if(desktop_lock_menu_dualboot_partition()) {
                /* The tile is the quick launcher: one discovered firmware
                 * boots directly; multiple choices become named buttons.
                 * Apps > Dual Boot opens the full install/delete manager. */
                loader_start_detached_with_gui_error(desktop->loader, "dualboot", "quick");
            } else {
                desktop_lock_menu_show_message(
                    desktop->lock_menu, "Dual Boot unavailable", "No secondary slot");
            }
            consumed = true;
            break;

        case DesktopLockMenuEventMeshClients:
            /* T-Embed ist immer Master; der Master-Mesh-Service läuft on-demand in
             * der Mesh-Clients-Scene. */
            scene_manager_next_scene(desktop->scene_manager, DesktopSceneMeshClients);
            consumed = true;
            break;

        case DesktopLockMenuEventDarkModeToggle:
            /* canvas.c inverts the whole UI on this flag; persist it. */
            momentum_settings.dark_mode = !momentum_settings.dark_mode;
            momentum_settings_save();
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventStealthToggle:
            desktop_set_stealth_mode_state(
                desktop, !furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode));
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventLock:
            desktop_lock(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventOpenSettings:
            /* Match Momentum: open the native Settings list. Its Desktop entry
             * owns the Current/Momentum Control Center selector. */
            loader_show_settings(desktop->loader);
            consumed = true;
            break;

        case DesktopLockMenuEventOpenMomentum:
            loader_start_detached_with_gui_error(desktop->loader, "momentum_app", NULL);
            consumed = true;
            break;

        case DesktopLockMenuEventOpenDisplay:
            loader_start_detached_with_gui_error(desktop->loader, "notification_settings", NULL);
            consumed = true;
            break;

        case DesktopLockMenuEventOpenPower:
            loader_start_detached_with_gui_error(desktop->loader, "power_settings", NULL);
            consumed = true;
            break;

        case DesktopLockMenuEventWebFs:
            /* Web-Filesystem lives in the WiFi app; launch it into that flow. */
            loader_start_detached_with_gui_error(desktop->loader, "wlan", "webfs");
            consumed = true;
            break;

        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        /* WiFi association and battery state change without a key event. */
        desktop_lock_menu_refresh_live(desktop->lock_menu);
        consumed = true;
    }

    return consumed;
}

void desktop_scene_lock_menu_on_exit(void* context) {
    Desktop* desktop = (Desktop*)context;
    gui_set_status_bar_hidden(desktop->gui, false);
}
