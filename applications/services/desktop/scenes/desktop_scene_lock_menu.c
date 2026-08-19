#include <furi.h>
#include <gui/scene_manager.h>

#include <btshim.h>

#include "../desktop_i.h"
#include "../views/desktop_view_lock_menu.h"
#include "../helpers/qflipper_bridge.h"

#include <notification/notification_messages.h>
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

/* Session state: Wake mode is a quick toggle, not a persisted setting. It is
 * held by the notification service's backlight lock, which has no getter. */
static bool s_wake_mode = false;

static bool desktop_lock_menu_bt_enabled(void) {
    Bt* bt = furi_record_open(RECORD_BT);
    BtSettings settings;
    bt_get_settings(bt, &settings);
    furi_record_close(RECORD_BT);
    return settings.enabled;
}

static void desktop_lock_menu_set_bt_enabled(bool enabled) {
    Bt* bt = furi_record_open(RECORD_BT);
    BtSettings settings;
    bt_get_settings(bt, &settings);
    settings.enabled = enabled;
    bt_set_settings(bt, &settings);
    furi_record_close(RECORD_BT);
}

/* The second firmware's slot. NULL on a single-app build, which is what hides
 * the menu row. Never the running partition: this port lives in factory. */
static const esp_partition_t* desktop_lock_menu_dualboot_partition(void) {
    const esp_partition_t* ota_0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);

    if(ota_0 && ota_0 == esp_ota_get_running_partition()) return NULL;
    return ota_0;
}

/* Rebuild the menu from the live toggle states (used on enter and after a
 * toggle, so the Enable/Disable labels track reality). */
static void desktop_scene_lock_menu_refresh(Desktop* desktop) {
    desktop_lock_menu_set_states(
        desktop->lock_menu,
        LOCK_MENU_USB_AVAILABLE,
        qflipper_bridge_is_active(),
        desktop_lock_menu_bt_enabled(),
        s_wake_mode,
        desktop_lock_menu_dualboot_partition() != NULL);
}

void desktop_scene_lock_menu_on_enter(void* context) {
    Desktop* desktop = (Desktop*)context;

    desktop_lock_menu_set_callback(desktop->lock_menu, desktop_scene_lock_menu_callback, desktop);
    desktop_scene_lock_menu_refresh(desktop);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdLockMenu);
}

bool desktop_scene_lock_menu_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = (Desktop*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case DesktopLockMenuEventQflipperToggle:
            if(qflipper_bridge_is_active()) {
                qflipper_bridge_stop();
            } else {
                qflipper_bridge_start();
            }
            /* Stay in the menu; refresh so the label flips. */
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
            desktop_lock_menu_set_bt_enabled(!desktop_lock_menu_bt_enabled());
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventWakeToggle:
            s_wake_mode = !s_wake_mode;
            notification_message(
                desktop->notification,
                s_wake_mode ? &sequence_display_backlight_enforce_on :
                              &sequence_display_backlight_enforce_auto);
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventDualBoot: {
            /* Point otadata at ota_0 and restart. Coming back is the other
             * firmware's job: esp_ota_set_boot_partition() on a *factory*
             * partition erases otadata rather than writing a slot number, and
             * an erased otadata boots factory -- so this port is always the
             * fallback direction. */
            const esp_partition_t* target = desktop_lock_menu_dualboot_partition();
            if(!target) {
                /* The slot vanished between building the menu and pressing OK. */
                desktop_lock_menu_show_message(
                    desktop->lock_menu, "Dual Boot unavailable", "No second slot");
                consumed = true;
                break;
            }

            esp_err_t err = esp_ota_set_boot_partition(target);
            if(err != ESP_OK) {
                /* Almost always ESP_ERR_OTA_VALIDATE_FAILED: the slot exists but
                 * is empty or holds a bad image. Say so -- refreshing silently
                 * looks identical to the button doing nothing. Staying put also
                 * beats rebooting into a partition the bootloader will reject. */
                FURI_LOG_E(
                    "DesktopLockMenu",
                    "Dual boot: cannot select ota_0: %s",
                    esp_err_to_name(err));
                if(err == ESP_ERR_OTA_VALIDATE_FAILED) {
                    /* Empty or bad slot -- the expected state until the user
                     * installs something of their own. */
                    desktop_lock_menu_show_message(
                        desktop->lock_menu, "Dual Boot unavailable", "Install firmware first");
                } else {
                    desktop_lock_menu_show_message(
                        desktop->lock_menu, "Cannot switch firmware", esp_err_to_name(err));
                }
                consumed = true;
                break;
            }

            FURI_LOG_I("DesktopLockMenu", "Dual boot: rebooting into ota_0");
            esp_restart();
            break;
        }

        case DesktopLockMenuEventMeshClients:
            /* T-Embed ist immer Master; der Master-Mesh-Service läuft on-demand in
             * der Mesh-Clients-Scene. */
            scene_manager_next_scene(desktop->scene_manager, DesktopSceneMeshClients);
            consumed = true;
            break;

        default:
            break;
        }
    }

    return consumed;
}

void desktop_scene_lock_menu_on_exit(void* context) {
    UNUSED(context);
}
