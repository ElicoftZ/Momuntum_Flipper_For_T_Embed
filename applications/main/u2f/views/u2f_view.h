#pragma once

#include <gui/view.h>

/* TEMPORARY: NFC mode is built but not offered. Set to 1 to put the "OK: NFC
 * mode" hint back on the unplugged screen and re-arm the OK press that enters
 * it. Everything behind it -- the screen, the worker, the diagnostics -- is
 * still compiled and still works. */
#define U2F_NFC_MODE_ENABLED 0

typedef struct U2fView U2fView;
typedef void (*U2fOkCallback)(InputType type, void* context);

typedef enum {
    U2fMsgNotConnected,
    U2fMsgIdle,
    U2fMsgRegister,
    U2fMsgAuth,
    U2fMsgSuccess,
    U2fMsgError,
    /* Answering over NFC instead of the cable. Only reachable while unplugged,
     * and left again with OK or Back. */
    U2fMsgNfc,
} U2fViewMsg;

U2fView* u2f_view_alloc(void);

void u2f_view_free(U2fView* u2f);

View* u2f_view_get_view(U2fView* u2f);

void u2f_view_set_ok_callback(U2fView* u2f, U2fOkCallback callback, void* context);

/* Up and Down do nothing on this screen otherwise, so Settings costs the
 * primary "plug in, press OK" flow nothing -- no menu in front of it. */
typedef void (*U2fSettingsCallback)(void* context);
void u2f_view_set_settings_callback(U2fView* u2f, U2fSettingsCallback callback, void* context);

void u2f_view_set_state(U2fView* u2f, U2fViewMsg msg);

/* What the NFC screen reports while it is up. Deliberately not FidoNfcStatus:
 * the view should not have to know what a PN532 is, and the scene is the right
 * place to decide which of these a given set of counters means. */
typedef enum {
    U2fNfcStatusStarting,
    U2fNfcStatusFailed, /* the radio never came up */
    U2fNfcStatusWaiting, /* listening, nothing has coupled yet */
    U2fNfcStatusCoupled, /* a reader talked to us but never asked for FIDO */
    U2fNfcStatusSelected, /* a reader selected the FIDO applet */
} U2fNfcStatus;

void u2f_view_set_nfc_status(U2fView* u2f, U2fNfcStatus status, uint32_t apdu_count);
