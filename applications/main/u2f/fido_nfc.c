#include "fido_nfc.h"

#include "ctap2.h"

#include <string.h>

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_nfc.h>

#define TAG "FidoNfc"

/* The FIDO applet identifier, fixed by the spec. */
static const uint8_t fido_aid[] = {0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01};

/* Answer to a successful SELECT. Historic, and still what clients expect even
 * from an authenticator that also speaks CTAP2. */
static const uint8_t fido_select_response[] = {'U', '2', 'F', '_', 'V', '2'};

/* ISO7816 status words. */
#define SW_NO_ERROR              0x9000
#define SW_BYTES_REMAINING       0x6100 /* | count still to collect */
#define SW_WRONG_LENGTH          0x6700
#define SW_CONDITIONS_NOT_SATISFIED 0x6985
#define SW_FILE_NOT_FOUND        0x6A82
#define SW_INS_NOT_SUPPORTED     0x6D00
#define SW_CLA_NOT_SUPPORTED     0x6E00

#define INS_SELECT       0xA4
#define INS_GET_RESPONSE 0xC0
#define INS_U2F_REGISTER 0x01
#define INS_U2F_AUTH     0x02
#define INS_U2F_VERSION  0x03
#define INS_CTAP2_MSG    0x10

#define CLA_CHAINING 0x10

/* Largest data field in one R-APDU. The PN532 frame ceiling is ~262 including
 * overhead, so this leaves comfortable room for the status word. */
#define APDU_MAX_CHUNK 240

/* A U2F register response runs to roughly 580 bytes; CTAP2 is capped by
 * maxMsgSize. */
#define FIDO_NFC_RESP_MAX (CTAP2_MAX_MSG_SIZE + 128)
#define FIDO_NFC_REQ_MAX  (CTAP2_MAX_MSG_SIZE + 64)

/* u2f_msg_parse() reads a U2F raw message: a 4-byte header, a 3-byte extended
 * length, then the payload. NFC delivers short APDUs with a 1-byte Lc, so the
 * command is re-framed into this scratch buffer before being handed over. It
 * also receives the response in place. */
#define FIDO_NFC_U2F_MAX 1024
#define U2F_RAW_HEADER_LEN 7

struct FidoNfc {
    FuriThread* thread;
    U2fData* u2f;
    Ctap2* ctap2;
    volatile bool running;
    volatile bool activated;
    volatile FidoNfcHalState hal_state;
    volatile bool ever_selected;
    volatile uint32_t apdu_count;

    /* Request chaining accumulator. */
    uint8_t req[FIDO_NFC_REQ_MAX];
    size_t req_len;
    bool chaining;

    /* Response awaiting collection via GET RESPONSE. */
    uint8_t resp[FIDO_NFC_RESP_MAX];
    size_t resp_len;
    size_t resp_sent;

    uint8_t u2f_buf[FIDO_NFC_U2F_MAX];

    bool selected;
};

/* Over NFC the card being in the field IS the user's presence: there is no
 * button to press while a phone is held to the antenna, and taking the phone
 * away is the cancel. */
static Ctap2PresenceResult fido_nfc_presence(void* context, U2fNotifyEvent prompt) {
    FidoNfc* instance = context;
    /* Still surfaced on screen so the user can see what the phone asked for. */
    u2f_notify(instance->u2f, prompt);
    u2f_confirm_user_present(instance->u2f);
    return Ctap2PresenceGranted;
}

/* ------------------------------------------------------------- APDU I/O */

static size_t fido_nfc_status(uint8_t* resp, uint16_t sw) {
    resp[0] = (uint8_t)(sw >> 8);
    resp[1] = (uint8_t)sw;
    return 2;
}

/* Hand over the next slice of a prepared response, appending 61xx when more
 * remains so the reader knows to send GET RESPONSE. */
static size_t fido_nfc_send_chunk(FidoNfc* instance, uint32_t le, uint8_t* resp, size_t resp_cap) {
    size_t remaining = instance->resp_len - instance->resp_sent;
    size_t chunk = remaining;

    if(le > 0 && chunk > le) chunk = le;
    if(chunk > APDU_MAX_CHUNK) chunk = APDU_MAX_CHUNK;
    if(chunk > resp_cap - 2) chunk = resp_cap - 2;

    memcpy(resp, instance->resp + instance->resp_sent, chunk);
    instance->resp_sent += chunk;

    size_t left = instance->resp_len - instance->resp_sent;
    if(left == 0) {
        instance->resp_len = 0;
        instance->resp_sent = 0;
        return chunk + fido_nfc_status(resp + chunk, SW_NO_ERROR);
    }

    /* 0x61 00 means "more than 255 left", which is a legal encoding. */
    uint16_t sw = (uint16_t)(SW_BYTES_REMAINING | ((left > 0xFF) ? 0x00 : left));
    return chunk + fido_nfc_status(resp + chunk, sw);
}

/* ------------------------------------------------------- command handling */

static size_t fido_nfc_handle_ctap2(FidoNfc* instance, uint8_t* resp, size_t resp_cap) {
    instance->resp_len = ctap2_request(
        instance->ctap2, instance->req, instance->req_len, instance->resp, sizeof(instance->resp));
    instance->resp_sent = 0;
    return fido_nfc_send_chunk(instance, 0, resp, resp_cap);
}

static size_t fido_nfc_handle_u2f(
    FidoNfc* instance,
    uint8_t ins,
    uint8_t p1,
    uint8_t p2,
    uint8_t* resp,
    size_t resp_cap) {
    if(instance->req_len + U2F_RAW_HEADER_LEN > sizeof(instance->u2f_buf)) {
        return fido_nfc_status(resp, SW_WRONG_LENGTH);
    }

    /* Re-frame the short APDU as the extended-length raw U2F message that
     * u2f_msg_parse expects. */
    instance->u2f_buf[0] = 0x00;
    instance->u2f_buf[1] = ins;
    instance->u2f_buf[2] = p1;
    instance->u2f_buf[3] = p2;
    instance->u2f_buf[4] = 0x00;
    instance->u2f_buf[5] = (uint8_t)(instance->req_len >> 8);
    instance->u2f_buf[6] = (uint8_t)instance->req_len;
    memcpy(instance->u2f_buf + U2F_RAW_HEADER_LEN, instance->req, instance->req_len);

    /* CTAP1 over USB answers "user missing" and lets the host retry. Over NFC
     * there is no button, so presence is granted up front -- the phone being
     * on the antenna is the gesture. */
    u2f_notify(instance->u2f, (ins == INS_U2F_REGISTER) ? U2fNotifyRegister : U2fNotifyAuth);
    u2f_confirm_user_present(instance->u2f);

    uint16_t len = u2f_msg_parse(
        instance->u2f, instance->u2f_buf, (uint16_t)(instance->req_len + U2F_RAW_HEADER_LEN));

    if(len == 0) return fido_nfc_status(resp, SW_CONDITIONS_NOT_SATISFIED);
    if(len > sizeof(instance->resp)) return fido_nfc_status(resp, SW_WRONG_LENGTH);

    /* u2f_msg_parse already appended its own status word. */
    memcpy(instance->resp, instance->u2f_buf, len);
    instance->resp_len = len;
    instance->resp_sent = 0;
    return fido_nfc_send_chunk(instance, 0, resp, resp_cap);
}

static size_t fido_nfc_apdu(
    void* context,
    const uint8_t* apdu,
    size_t apdu_len,
    uint8_t* resp,
    size_t resp_cap) {
    FidoNfc* instance = context;

    /* Any APDU at all means a reader activated the emulated card -- the single
     * most useful thing to know when a phone tap does nothing, because it
     * separates "never coupled" from "coupled but the applet said no". */
    instance->activated = true;
    instance->apdu_count++;

    if(apdu_len < 4 || resp_cap < 2) return 0;

    const uint8_t cla = apdu[0];
    const uint8_t ins = apdu[1];
    const uint8_t p1 = apdu[2];
    const uint8_t p2 = apdu[3];

    /* Only the interindustry class, with or without the chaining bit. */
    if((cla & 0xEF) != 0x00 && (cla & 0xEF) != 0x80) {
        return fido_nfc_status(resp, SW_CLA_NOT_SUPPORTED);
    }

    /* Decode Lc / data / Le. Short form only -- an extended-length APDU cannot
     * reach us through a 262-byte PN532 frame anyway. */
    const uint8_t* data = NULL;
    size_t lc = 0;
    uint32_t le = 0;

    if(apdu_len == 4) {
        /* Case 1: no data, no response expected. */
    } else if(apdu_len == 5) {
        le = (apdu[4] == 0) ? 256 : apdu[4];
    } else if(apdu[4] != 0) {
        lc = apdu[4];
        if(5 + lc > apdu_len) return fido_nfc_status(resp, SW_WRONG_LENGTH);
        data = apdu + 5;
        if(apdu_len > 5 + lc) le = (apdu[5 + lc] == 0) ? 256 : apdu[5 + lc];
    } else if(apdu_len >= 7) {
        /* Extended form, which some readers emit even for short payloads. */
        lc = ((size_t)apdu[5] << 8) | apdu[6];
        if(7 + lc > apdu_len) return fido_nfc_status(resp, SW_WRONG_LENGTH);
        data = apdu + 7;
        if(apdu_len >= 9 + lc) le = ((uint32_t)apdu[7 + lc] << 8) | apdu[8 + lc];
        if(le == 0) le = 65536;
    } else {
        return fido_nfc_status(resp, SW_WRONG_LENGTH);
    }

    /* GET RESPONSE is answered before anything else -- it is not a command in
     * its own right, just collection of the previous one's output. */
    if(ins == INS_GET_RESPONSE) {
        if(instance->resp_len == 0) return fido_nfc_status(resp, SW_CONDITIONS_NOT_SATISFIED);
        return fido_nfc_send_chunk(instance, le, resp, resp_cap);
    }

    /* Any new command abandons an uncollected response. */
    instance->resp_len = 0;
    instance->resp_sent = 0;

    if(ins == INS_SELECT) {
        instance->chaining = false;
        instance->req_len = 0;

        if(p1 != 0x04 || data == NULL || lc != sizeof(fido_aid) ||
           memcmp(data, fido_aid, sizeof(fido_aid)) != 0) {
            instance->selected = false;
            return fido_nfc_status(resp, SW_FILE_NOT_FOUND);
        }

        instance->selected = true;
        instance->ever_selected = true;
        if(resp_cap < sizeof(fido_select_response) + 2) return 0;
        memcpy(resp, fido_select_response, sizeof(fido_select_response));
        return sizeof(fido_select_response) +
               fido_nfc_status(resp + sizeof(fido_select_response), SW_NO_ERROR);
    }

    if(!instance->selected) return fido_nfc_status(resp, SW_CONDITIONS_NOT_SATISFIED);

    /* Accumulate the request, whether it arrives in one APDU or several. */
    if(!instance->chaining) instance->req_len = 0;
    if(lc > 0) {
        if(instance->req_len + lc > sizeof(instance->req)) {
            instance->chaining = false;
            instance->req_len = 0;
            return fido_nfc_status(resp, SW_WRONG_LENGTH);
        }
        memcpy(instance->req + instance->req_len, data, lc);
        instance->req_len += lc;
    }

    if(cla & CLA_CHAINING) {
        /* More to come: acknowledge and wait for the rest. */
        instance->chaining = true;
        return fido_nfc_status(resp, SW_NO_ERROR);
    }
    instance->chaining = false;

    switch(ins) {
    case INS_CTAP2_MSG:
        if(instance->req_len < 1) return fido_nfc_status(resp, SW_WRONG_LENGTH);
        return fido_nfc_handle_ctap2(instance, resp, resp_cap);

    case INS_U2F_REGISTER:
    case INS_U2F_AUTH:
    case INS_U2F_VERSION:
        return fido_nfc_handle_u2f(instance, ins, p1, p2, resp, resp_cap);

    default:
        return fido_nfc_status(resp, SW_INS_NOT_SUPPORTED);
    }
}

/* ------------------------------------------------------------- worker */

static int32_t fido_nfc_worker(void* context) {
    FidoNfc* instance = context;

    if(furi_hal_nfc_acquire() != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "Could not acquire the NFC HAL");
        /* Reported on screen: this used to be a log line nobody could read,
         * because the composite owns the USB PHY while U2F is open. */
        instance->hal_state = FidoNfcHalFailed;
        return 0;
    }

    furi_hal_nfc_low_power_mode_stop();
    furi_hal_nfc_set_mode(FuriHalNfcModeListener, FuriHalNfcTechIso14443a);
    furi_hal_nfc_emu_set_apdu_handler(fido_nfc_apdu, instance);

    instance->hal_state = FidoNfcHalReady;
    FURI_LOG_I(TAG, "FIDO NFC applet listening");

    /* The HAL loop runs until aborted, which fido_nfc_stop does. */
    while(instance->running) {
        FuriHalNfcEvent event =
            furi_hal_nfc_listener_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
        if(event == FuriHalNfcEventAbortRequest) break;
    }

    furi_hal_nfc_emu_set_apdu_handler(NULL, NULL);
    furi_hal_nfc_reset_mode();
    furi_hal_nfc_low_power_mode_start();
    furi_hal_nfc_release();

    FURI_LOG_I(TAG, "FIDO NFC applet stopped");
    return 0;
}

FidoNfc* fido_nfc_start(U2fData* u2f) {
    furi_assert(u2f);

    FidoNfc* instance = calloc(1, sizeof(FidoNfc));
    instance->u2f = u2f;

    /* Its own CTAP2 instance, because the presence rule differs from USB:
     * here the field IS the presence. Both instances share the same U2fData,
     * so the device key, counter and credentials are common -- which is what
     * makes a credential registered over USB work over NFC. */
    instance->ctap2 = ctap2_alloc(u2f);
    ctap2_set_presence_callback(instance->ctap2, fido_nfc_presence, instance);

    instance->running = true;
    /* Same 8 KB as the USB worker: the deepest call is still mbedtls ECDSA
     * plus CBOR assembly. */
    instance->thread = furi_thread_alloc_ex("FidoNfcWorker", 8192, fido_nfc_worker, instance);
    furi_thread_start(instance->thread);

    return instance;
}

void fido_nfc_stop(FidoNfc* instance) {
    furi_assert(instance);

    instance->running = false;
    /* Disarm first so the loop cannot start another TgGetData, then abort to
     * break it out of the one it is in. */
    furi_hal_nfc_emu_set_apdu_handler(NULL, NULL);
    furi_hal_nfc_abort();

    furi_thread_join(instance->thread);
    furi_thread_free(instance->thread);

    ctap2_free(instance->ctap2);
    memset(instance, 0, sizeof(FidoNfc));
    free(instance);
}

void fido_nfc_get_status(const FidoNfc* instance, FidoNfcStatus* out) {
    furi_assert(out);

    if(instance == NULL) {
        out->hal = FidoNfcHalStarting;
        out->activated = false;
        out->selected = false;
        out->apdu_count = 0;
        return;
    }

    out->hal = instance->hal_state;
    out->activated = instance->activated;
    out->selected = instance->ever_selected;
    out->apdu_count = instance->apdu_count;
}
