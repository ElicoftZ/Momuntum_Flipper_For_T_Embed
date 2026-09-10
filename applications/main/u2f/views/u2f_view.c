#include "u2f_view.h"
#include <stdio.h>
#include <gui/elements.h>
#include <gui/icon_animation.h>
#include <assets_icons.h>

struct U2fView {
    View* view;
    U2fOkCallback callback;
    void* context;
    U2fSettingsCallback settings_callback;
    void* settings_context;
};

typedef struct {
    U2fViewMsg display_msg;
    /* Only spins while the NFC screen is up; the draw callback is handed the
     * model and nothing else, so it has to live here rather than in U2fView. */
    IconAnimation* icon;
    U2fNfcStatus nfc_status;
    uint32_t nfc_apdu_count;
} U2fModel;

static void u2f_view_draw_callback(Canvas* canvas, void* _model) {
    U2fModel* model = _model;

    if(model->display_msg == U2fMsgNfc) {
        /* Same furniture as the NFC app's read screen, so "hold it against the
         * reader" reads the same way here as it does there. */
        canvas_draw_icon(canvas, 0, 13, &I_NFC_manual_60x50);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 128 / 2, 3, AlignCenter, AlignTop, "NFC mode");
        canvas_set_font(canvas, FontSecondary);

        /* Two short lines rather than one long one: the tag artwork takes the
         * left 60px, leaving about 11 characters per line. */
        const char* line1 = "Waiting for";
        const char* line2 = "a reader";
        if(model->nfc_status == U2fNfcStatusStarting) {
            line1 = "Starting";
            line2 = "the radio";
        } else if(model->nfc_status == U2fNfcStatusFailed) {
            line1 = "NFC radio";
            line2 = "unavailable";
        } else if(model->nfc_status == U2fNfcStatusCoupled) {
            /* The reader is there and talking, but has not asked for FIDO --
             * usually a plain tag scan rather than a WebAuthn ceremony. */
            line1 = "Reader seen";
            line2 = "no FIDO yet";
        } else if(model->nfc_status == U2fNfcStatusSelected) {
            line1 = "FIDO reader";
            line2 = "connected";
        }

        canvas_draw_str_aligned(canvas, 62, 20, AlignLeft, AlignTop, line1);
        canvas_draw_str_aligned(canvas, 62, 30, AlignLeft, AlignTop, line2);

        /* The loader only spins while there is genuinely nothing to report. */
        if(model->nfc_status == U2fNfcStatusWaiting) {
            canvas_draw_icon_animation(canvas, 116, 20, model->icon);
        }

        if(model->nfc_apdu_count > 0) {
            /* "APDUs: " plus a full uint32_t and the NUL. */
            char count[24];
            snprintf(count, sizeof(count), "APDUs: %lu", (unsigned long)model->nfc_apdu_count);
            canvas_draw_str_aligned(canvas, 62, 42, AlignLeft, AlignTop, count);
        }

        canvas_draw_str_aligned(canvas, 62, 54, AlignLeft, AlignTop, "OK: USB mode");
        return;
    }

    canvas_draw_icon(canvas, 8, 14, &I_Drive_112x35);
    canvas_set_font(canvas, FontSecondary);

    if(model->display_msg == U2fMsgNotConnected) {
        canvas_draw_icon(canvas, 22, 15, &I_Connect_me_62x31);
        canvas_draw_str_aligned(
            canvas, 128 / 2, 3, AlignCenter, AlignTop, "Connect me to computer");
    } else if(model->display_msg == U2fMsgIdle) {
        canvas_draw_icon(canvas, 22, 15, &I_Connected_62x31);
        canvas_draw_str_aligned(canvas, 128 / 2, 3, AlignCenter, AlignTop, "Connected!");
    } else if(model->display_msg == U2fMsgRegister) {
        elements_button_center(canvas, "OK");
        canvas_draw_icon(canvas, 22, 15, &I_Auth_62x31);
        canvas_draw_str_aligned(canvas, 128 / 2, 3, AlignCenter, AlignTop, "Press OK to register");
    } else if(model->display_msg == U2fMsgAuth) {
        elements_button_center(canvas, "OK");
        canvas_draw_icon(canvas, 22, 15, &I_Auth_62x31);
        canvas_draw_str_aligned(
            canvas, 128 / 2, 3, AlignCenter, AlignTop, "Press OK to authenticate");
    } else if(model->display_msg == U2fMsgSuccess) {
        canvas_draw_icon(canvas, 22, 15, &I_Connected_62x31);
        canvas_draw_str_aligned(
            canvas, 128 / 2, 3, AlignCenter, AlignTop, "Authentication successful!");
    } else if(model->display_msg == U2fMsgError) {
        canvas_draw_icon(canvas, 22, 15, &I_Error_62x31);
        canvas_draw_str_aligned(canvas, 128 / 2, 3, AlignCenter, AlignTop, "Certificate error");
    }

#if U2F_NFC_MODE_ENABLED
    if(model->display_msg == U2fMsgNotConnected) {
        /* NFC is offered only while unplugged: with a host attached the cable
         * is the transport, and two live at once would leave the user guessing
         * which one answered. The drive artwork ends at y=48, so both hints
         * fit below it. */
        canvas_draw_str_aligned(canvas, 64, 49, AlignCenter, AlignTop, "Up/Down: Settings");
        canvas_draw_str_aligned(canvas, 64, 57, AlignCenter, AlignTop, "OK: NFC mode");
    } else if(model->display_msg == U2fMsgIdle) {
        canvas_draw_str_aligned(canvas, 64, 51, AlignCenter, AlignTop, "Up/Down: Settings");
    }
#else
    if(model->display_msg == U2fMsgIdle || model->display_msg == U2fMsgNotConnected) {
        canvas_draw_str_aligned(canvas, 64, 51, AlignCenter, AlignTop, "Up/Down: Settings");
    }
#endif
}

static bool u2f_view_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    U2fView* u2f = context;
    bool consumed = false;

    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk) {
            consumed = true;
            if(u2f->callback != NULL) u2f->callback(InputTypeShort, u2f->context);
        } else if(event->key == InputKeyUp || event->key == InputKeyDown) {
            consumed = true;
            if(u2f->settings_callback != NULL) u2f->settings_callback(u2f->settings_context);
        }
    }

    return consumed;
}

U2fView* u2f_view_alloc(void) {
    U2fView* u2f = calloc(1, sizeof(U2fView));

    u2f->view = view_alloc();
    view_allocate_model(u2f->view, ViewModelTypeLocking, sizeof(U2fModel));
    view_set_context(u2f->view, u2f);
    view_set_draw_callback(u2f->view, u2f_view_draw_callback);
    view_set_input_callback(u2f->view, u2f_view_input_callback);

    with_view_model(
        u2f->view,
        U2fModel * model,
        {
            model->icon = icon_animation_alloc(&A_Round_loader_8x8);
            view_tie_icon_animation(u2f->view, model->icon);
        },
        false);

    return u2f;
}

void u2f_view_free(U2fView* u2f) {
    furi_assert(u2f);
    with_view_model(
        u2f->view, U2fModel * model, { icon_animation_free(model->icon); }, false);
    view_free(u2f->view);
    free(u2f);
}

View* u2f_view_get_view(U2fView* u2f) {
    furi_assert(u2f);
    return u2f->view;
}

void u2f_view_set_ok_callback(U2fView* u2f, U2fOkCallback callback, void* context) {
    furi_assert(u2f);
    furi_assert(callback);
    with_view_model(
        u2f->view,
        U2fModel * model,
        {
            UNUSED(model);
            u2f->callback = callback;
            u2f->context = context;
        },
        false);
}

void u2f_view_set_settings_callback(U2fView* u2f, U2fSettingsCallback callback, void* context) {
    furi_assert(u2f);
    u2f->settings_callback = callback;
    u2f->settings_context = context;
}

void u2f_view_set_nfc_status(U2fView* u2f, U2fNfcStatus status, uint32_t apdu_count) {
    furi_assert(u2f);
    with_view_model(
        u2f->view,
        U2fModel * model,
        {
            model->nfc_status = status;
            model->nfc_apdu_count = apdu_count;
        },
        true);
}

void u2f_view_set_state(U2fView* u2f, U2fViewMsg msg) {
    IconAnimation* icon = NULL;
    with_view_model(
        u2f->view,
        U2fModel * model,
        {
            model->display_msg = msg;
            icon = model->icon;
        },
        true);

    /* Outside the model lock: the animation's frame timer redraws through that
     * same lock. Both calls are idempotent, so repeated states are free. */
    if(msg == U2fMsgNfc) {
        icon_animation_start(icon);
    } else {
        icon_animation_stop(icon);
    }
}
