#include "../u2f_app_i.h"
#include "../ctap2_rk.h"
#include "../ctap2_pin.h"

#include <stdio.h>
#include <string.h>

/* The list of saved sites is itself sensitive -- it reveals which services the
 * owner uses, which is exactly the kind of thing someone who picks the board up
 * should not get for free. So when a PIN is set it is required every time this
 * screen is opened, not once per session.
 *
 * Deleting is deliberately two taps deep. There is no undo: the record holds
 * the only copy of which account a discoverable credential belongs to, so
 * removing it makes that passkey unusable even though the site still lists it.
 */

typedef enum {
    U2fCredsStateLocked,
    U2fCredsStateListing,
    U2fCredsStateMenu,
    U2fCredsStateConfirm,
} U2fCredsState;

/* Submenu indices arrive as custom events, and this scene also receives the
 * PIN events from GpioCustomEvent. Rows are offset clear of those so that the
 * fourteenth site cannot show up looking like U2fCustomEventPinEntered. */
#define U2F_CREDS_EVENT_ROW_BASE 0x100
#define U2F_CREDS_EVENT_DELETE   0x200
#define U2F_CREDS_EVENT_CONFIRM  0x201
#define U2F_CREDS_EVENT_BACK     0x202

static void u2f_scene_creds_submenu_callback(void* context, uint32_t index) {
    furi_assert(context);
    U2fApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

/* Site first: it is what the owner is scanning for. The account only matters
 * when there is more than one at the same site. */
static void u2f_scene_creds_format_label(const Ctap2RkRecord* record, char* out, size_t out_len) {
    if(record->user_name[0] != 0) {
        snprintf(out, out_len, "%s: %s", record->rp_id, record->user_name);
    } else {
        snprintf(out, out_len, "%s", record->rp_id);
    }
}

static void u2f_scene_creds_build_list(U2fApp* app) {
    scene_manager_set_scene_state(app->scene_manager, U2fSceneCreds, U2fCredsStateListing);
    submenu_reset(app->submenu);

    size_t total = ctap2_rk_count_all();
    if(total > CTAP2_RK_MAX_CREDENTIALS) total = CTAP2_RK_MAX_CREDENTIALS;

    if(total == 0) {
        submenu_set_header(app->submenu, "No sites saved");
    } else {
        snprintf(
            app->label_creds_header,
            sizeof(app->label_creds_header),
            "Saved sites (%u)",
            (unsigned)total);
        submenu_set_header(app->submenu, app->label_creds_header);
    }

    for(size_t i = 0; i < total; i++) {
        Ctap2RkRecord record;
        if(!ctap2_rk_get_any(i, &record)) continue;

        /* Sized for the data, not the screen: two CTAP2_RK_NAME_MAX fields plus
         * the separator. The submenu does its own visual truncation. */
        char label[CTAP2_RK_NAME_MAX * 2 + 4];
        u2f_scene_creds_format_label(&record, label, sizeof(label));

        submenu_add_item(
            app->submenu,
            label,
            (uint32_t)(U2F_CREDS_EVENT_ROW_BASE + i),
            u2f_scene_creds_submenu_callback,
            app);
        memset(&record, 0, sizeof(record));
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, U2fAppViewSettings);
}

/* What to do with the site the user just picked. The name stays in the header
 * through both steps so there is never a doubt about which one is going. */
static void u2f_scene_creds_build_menu(U2fApp* app) {
    scene_manager_set_scene_state(app->scene_manager, U2fSceneCreds, U2fCredsStateMenu);
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, app->label_creds_item);
    submenu_add_item(
        app->submenu, "Delete", U2F_CREDS_EVENT_DELETE, u2f_scene_creds_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Back", U2F_CREDS_EVENT_BACK, u2f_scene_creds_submenu_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, U2fAppViewSettings);
}

static void u2f_scene_creds_build_confirm(U2fApp* app) {
    scene_manager_set_scene_state(app->scene_manager, U2fSceneCreds, U2fCredsStateConfirm);
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Delete permanently?");
    submenu_add_item(
        app->submenu,
        "Yes, delete",
        U2F_CREDS_EVENT_CONFIRM,
        u2f_scene_creds_submenu_callback,
        app);
    submenu_add_item(
        app->submenu, "No, keep it", U2F_CREDS_EVENT_BACK, u2f_scene_creds_submenu_callback, app);
    /* Cancel starts selected: a stray double-press must not delete anything. */
    submenu_set_selected_item(app->submenu, 1);
    view_dispatcher_switch_to_view(app->view_dispatcher, U2fAppViewSettings);
}

static void u2f_scene_creds_pin_done_callback(const char* pin, void* context) {
    furi_assert(context);
    U2fApp* app = context;

    strncpy(app->pin_entry, pin, sizeof(app->pin_entry) - 1);
    app->pin_entry[sizeof(app->pin_entry) - 1] = 0;
    view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventPinEntered);
}

static void u2f_scene_creds_pin_back_callback(void* context) {
    furi_assert(context);
    U2fApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, U2fCustomEventPinCancelled);
}

void u2f_scene_creds_on_enter(void* context) {
    U2fApp* app = context;

    if(ctap2_pin_is_set()) {
        scene_manager_set_scene_state(app->scene_manager, U2fSceneCreds, U2fCredsStateLocked);

        u2f_pin_input_set_callbacks(
            app->pin_input,
            u2f_scene_creds_pin_done_callback,
            u2f_scene_creds_pin_back_callback,
            app);
        u2f_pin_input_reset(app->pin_input, "PIN to view sites");

        if(ctap2_pin_get_retries() == 0) {
            u2f_pin_input_set_error(app->pin_input, "PIN blocked - reset needed");
        }

        view_dispatcher_switch_to_view(app->view_dispatcher, U2fAppViewPinInput);
        return;
    }

    u2f_scene_creds_build_list(app);
}

bool u2f_scene_creds_on_event(void* context, SceneManagerEvent event) {
    U2fApp* app = context;
    bool consumed = false;
    uint32_t state = scene_manager_get_scene_state(app->scene_manager, U2fSceneCreds);

    if(event.type == SceneManagerEventTypeBack) {
        /* Back steps out of the delete flow one stage at a time; only the list
         * itself hands the press on to the scene manager. */
        if(state == U2fCredsStateConfirm) {
            u2f_scene_creds_build_menu(app);
            consumed = true;
        } else if(state == U2fCredsStateMenu) {
            u2f_scene_creds_build_list(app);
            consumed = true;
        }

    } else if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == U2fCustomEventPinEntered) {
            /* Spends a retry on failure, same as the host path -- an unlock
             * screen that guessed for free would undermine the counter that
             * protects everything else. */
            bool ok = ctap2_pin_check((const uint8_t*)app->pin_entry, strlen(app->pin_entry));
            memset(app->pin_entry, 0, sizeof(app->pin_entry));

            if(ok) {
                u2f_scene_creds_build_list(app);
            } else {
                uint8_t retries = ctap2_pin_get_retries();
                u2f_pin_input_set_error(
                    app->pin_input, (retries == 0) ? "Blocked - reset needed" : "Wrong PIN");
            }
            consumed = true;

        } else if(event.event == U2fCustomEventPinCancelled) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;

        } else if(event.event == U2F_CREDS_EVENT_BACK) {
            if(state == U2fCredsStateConfirm) {
                u2f_scene_creds_build_menu(app);
            } else {
                u2f_scene_creds_build_list(app);
            }
            consumed = true;

        } else if(event.event == U2F_CREDS_EVENT_DELETE) {
            u2f_scene_creds_build_confirm(app);
            consumed = true;

        } else if(event.event == U2F_CREDS_EVENT_CONFIRM) {
            ctap2_rk_delete(
                app->creds_selected.rp_id_hash,
                app->creds_selected.user_id,
                app->creds_selected.user_id_len);
            memset(&app->creds_selected, 0, sizeof(app->creds_selected));
            /* Rebuilt from the directory rather than patched, so what is on
             * screen is what is actually on the card. */
            u2f_scene_creds_build_list(app);
            consumed = true;

        } else if(event.event >= U2F_CREDS_EVENT_ROW_BASE && state == U2fCredsStateListing) {
            size_t index = event.event - U2F_CREDS_EVENT_ROW_BASE;
            if(ctap2_rk_get_any(index, &app->creds_selected)) {
                u2f_scene_creds_format_label(
                    &app->creds_selected, app->label_creds_item, sizeof(app->label_creds_item));
                u2f_scene_creds_build_menu(app);
            } else {
                /* The row went away underneath us -- a host can register or
                 * replace credentials while this screen is up. */
                u2f_scene_creds_build_list(app);
            }
            consumed = true;
        }
    }

    return consumed;
}

void u2f_scene_creds_on_exit(void* context) {
    U2fApp* app = context;
    u2f_pin_input_set_callbacks(app->pin_input, NULL, NULL, NULL);
    memset(app->pin_entry, 0, sizeof(app->pin_entry));
    memset(&app->creds_selected, 0, sizeof(app->creds_selected));
    submenu_reset(app->submenu);
    scene_manager_set_scene_state(app->scene_manager, U2fSceneCreds, U2fCredsStateLocked);
}
