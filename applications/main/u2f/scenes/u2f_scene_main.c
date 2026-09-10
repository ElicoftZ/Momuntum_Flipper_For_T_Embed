#include "../u2f_app_i.h"
#include "../views/u2f_view.h"
#include <dolphin/dolphin.h>
#include <furi_hal.h>
#include <furi_hal_usb_hid_u2f.h>
#include "../u2f.h"

/* Scene state, distinguishing "left for good" from "Settings pushed on top".
 * The scene manager runs on_exit in both cases. */
#define U2fSceneMainActive    0
#define U2fSceneMainSuspended 1

#define U2F_REQUEST_TIMEOUT 500
#define U2F_SUCCESS_TIMEOUT 3000

static void u2f_scene_main_ok_callback(InputType type, void* context) {
    UNUSED(type);
    furi_assert(context);
    U2fApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventConfirm);
}

static void u2f_scene_main_settings_callback(void* context) {
    furi_assert(context);
    U2fApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventOpenSettings);
}

static void u2f_scene_main_event_callback(U2fNotifyEvent evt, void* context) {
    furi_assert(context);
    U2fApp* app = context;
    if(evt == U2fNotifyRegister)
        view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventRegister);
    else if(evt == U2fNotifyAuth)
        view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventAuth);
    else if(evt == U2fNotifyAuthSuccess)
        view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventAuthSuccess);
    else if(evt == U2fNotifyWink)
        view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventWink);
    else if(evt == U2fNotifyConnect)
        view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventConnect);
    else if(evt == U2fNotifyDisconnect)
        view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventDisconnect);
    else if(evt == U2fNotifyError)
        view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventDataError);
}

static void u2f_scene_main_timer_callback(void* context) {
    furi_assert(context);
    U2fApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventTimeout);
}

/* Where the screen lands once a prompt clears: the NFC screen if that is what
 * is running, otherwise whatever the cable is doing. */
static void u2f_scene_main_show_idle(U2fApp* app) {
    if(app->nfc_mode) {
        u2f_view_set_state(app->u2f_view, U2fMsgNfc);
    } else {
        u2f_view_set_state(
            app->u2f_view, furi_hal_hid_u2f_is_connected() ? U2fMsgIdle : U2fMsgNotConnected);
    }
}

#if U2F_NFC_MODE_ENABLED
/* NFC costs a thread and keeps the reader out of low power, so it runs only
 * while the user has asked for it -- not for the whole time the app is open. */
static void u2f_scene_main_nfc_start(U2fApp* app) {
    if(app->fido_nfc == NULL) app->fido_nfc = fido_nfc_start(app->u2f_instance);
    app->nfc_mode = true;
    /* Cleared here rather than on exit, so the screen never opens showing the
     * previous session's counters. */
    u2f_view_set_nfc_status(app->u2f_view, U2fNfcStatusStarting, 0);
    u2f_view_set_state(app->u2f_view, U2fMsgNfc);
}
#endif

/* Translate the applet's counters into the one line the screen has room for.
 * Selected beats coupled beats waiting: the most specific thing that has
 * happened is the informative one. */
static void u2f_scene_main_nfc_poll(U2fApp* app) {
    FidoNfcStatus status;
    fido_nfc_get_status(app->fido_nfc, &status);

    U2fNfcStatus shown;
    if(status.hal == FidoNfcHalFailed) {
        shown = U2fNfcStatusFailed;
    } else if(status.hal == FidoNfcHalStarting) {
        shown = U2fNfcStatusStarting;
    } else if(status.selected) {
        shown = U2fNfcStatusSelected;
    } else if(status.activated) {
        shown = U2fNfcStatusCoupled;
    } else {
        shown = U2fNfcStatusWaiting;
    }

    u2f_view_set_nfc_status(app->u2f_view, shown, status.apdu_count);
}

static void u2f_scene_main_nfc_stop(U2fApp* app) {
    if(app->fido_nfc != NULL) {
        fido_nfc_stop(app->fido_nfc);
        app->fido_nfc = NULL;
    }
    app->nfc_mode = false;
}

bool u2f_scene_main_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    U2fApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        /* Nothing tells TinyUSB the cable was pulled on this board -- see
         * furi_hal_hid_u2f_poll_connection(). Without this the view stays on
         * "Connected!" after an unplug. */
        furi_hal_hid_u2f_poll_connection();
        if(app->nfc_mode) u2f_scene_main_nfc_poll(app);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == U2fCustomEventConnect) {
            furi_timer_stop(app->timer);
            /* The cable wins. NFC was only offered because there was no host,
             * and the option comes off the screen now that there is one. */
            u2f_scene_main_nfc_stop(app);
            u2f_view_set_state(app->u2f_view, U2fMsgIdle);
        } else if(event.event == U2fCustomEventDisconnect) {
            furi_timer_stop(app->timer);
            app->event_cur = U2fCustomEventNone;
            u2f_view_set_state(app->u2f_view, U2fMsgNotConnected);
        } else if((event.event == U2fCustomEventRegister) || (event.event == U2fCustomEventAuth)) {
            furi_timer_start(app->timer, U2F_REQUEST_TIMEOUT);
            if(app->event_cur == U2fCustomEventNone) {
                app->event_cur = event.event;
                if(event.event == U2fCustomEventRegister)
                    u2f_view_set_state(app->u2f_view, U2fMsgRegister);
                else if(event.event == U2fCustomEventAuth) //-V547
                    u2f_view_set_state(app->u2f_view, U2fMsgAuth);
                notification_message(app->notifications, &sequence_display_backlight_on);
                notification_message(app->notifications, &sequence_single_vibro);
            }
            notification_message(app->notifications, &sequence_blink_magenta_10);
        } else if(event.event == U2fCustomEventWink) {
            notification_message(app->notifications, &sequence_blink_magenta_10);
        } else if(event.event == U2fCustomEventAuthSuccess) {
            notification_message_block(app->notifications, &sequence_set_green_255);
            dolphin_deed(DolphinDeedU2fAuthorized);
            furi_timer_start(app->timer, U2F_SUCCESS_TIMEOUT);
            app->event_cur = U2fCustomEventNone;
            u2f_view_set_state(app->u2f_view, U2fMsgSuccess);
        } else if(event.event == U2fCustomEventTimeout) {
            notification_message_block(app->notifications, &sequence_reset_rgb);
            app->event_cur = U2fCustomEventNone;
            u2f_scene_main_show_idle(app);
        } else if(event.event == U2fCustomEventConfirm) {
            /* One button, three jobs, in priority order: answer a pending
             * request, leave NFC mode, or -- only with nothing plugged in --
             * enter it. The last one is switched off for now; the other two
             * still work, so a mode entered before the switch flipped can
             * still be left. */
            if(app->event_cur != U2fCustomEventNone) {
                u2f_confirm_user_present(app->u2f_instance);
            } else if(app->nfc_mode) {
                u2f_scene_main_nfc_stop(app);
                u2f_scene_main_show_idle(app);
            }
#if U2F_NFC_MODE_ENABLED
            else if(app->u2f_ready && !furi_hal_hid_u2f_is_connected()) {
                u2f_scene_main_nfc_start(app);
            }
#endif
        } else if(event.event == U2fCustomEventOpenSettings) {
            /* Mark this scene suspended rather than finished, so on_exit
             * leaves the running U2F stack alone. */
            scene_manager_set_scene_state(app->scene_manager, U2fSceneMain, U2fSceneMainSuspended);
            scene_manager_next_scene(app->scene_manager, U2fSceneSettings);
        } else if(event.event == U2fCustomEventDataError) {
            notification_message(app->notifications, &sequence_set_red_255);
            furi_timer_stop(app->timer);
            u2f_view_set_state(app->u2f_view, U2fMsgError);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        /* In NFC mode Back means "leave the mode", not "quit U2F" -- otherwise
         * getting back to the USB screen would mean reopening the app. */
        if(app->nfc_mode) {
            u2f_scene_main_nfc_stop(app);
            u2f_scene_main_show_idle(app);
            consumed = true;
        }
    }

    return consumed;
}

void u2f_scene_main_on_enter(void* context) {
    U2fApp* app = context;

    /* Coming back from Settings re-enters this scene. The U2F stack is still
     * up and its worker still owns the USB interface; allocating a second one
     * would leak the first and fight it for every frame. */
    if(scene_manager_get_scene_state(app->scene_manager, U2fSceneMain) ==
       U2fSceneMainSuspended) {
        scene_manager_set_scene_state(app->scene_manager, U2fSceneMain, U2fSceneMainActive);
        view_dispatcher_switch_to_view(app->view_dispatcher, U2fAppViewMain);
        return;
    }

    app->timer = furi_timer_alloc(u2f_scene_main_timer_callback, FuriTimerTypeOnce, app);

    app->u2f_instance = u2f_alloc();
    app->u2f_ready = u2f_init(app->u2f_instance);
    if(app->u2f_ready == true) {
        u2f_set_event_callback(app->u2f_instance, u2f_scene_main_event_callback, app);
        app->u2f_hid = u2f_hid_start(app->u2f_instance);

        /* NFC is not brought up here. It is a mode the user enters with OK
         * from the unplugged screen, so that the board answers on exactly the
         * transport that is on screen. */
        u2f_view_set_ok_callback(app->u2f_view, u2f_scene_main_ok_callback, app);
        u2f_view_set_settings_callback(app->u2f_view, u2f_scene_main_settings_callback, app);
    } else {
        u2f_free(app->u2f_instance);
        u2f_view_set_state(app->u2f_view, U2fMsgError);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, U2fAppViewMain);
}

void u2f_scene_main_on_exit(void* context) {
    U2fApp* app = context;

    /* Only tear down when the scene is really being left, not when Settings is
     * stacked on top of it -- the scene manager calls on_exit either way. */
    if(scene_manager_get_scene_state(app->scene_manager, U2fSceneMain) ==
       U2fSceneMainSuspended)
        return;

    notification_message_block(app->notifications, &sequence_reset_rgb);
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    app->timer = NULL;
    if(app->u2f_ready == true) {
        /* NFC first: its worker borrows the same U2fData. */
        u2f_scene_main_nfc_stop(app);
        u2f_hid_stop(app->u2f_hid);
        u2f_free(app->u2f_instance);
    }
}
