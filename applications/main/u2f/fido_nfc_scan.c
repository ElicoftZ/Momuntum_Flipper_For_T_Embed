#include "fido_nfc_scan.h"
#include "fido_cbor.h"

#include <string.h>

#include <furi.h>

#include <nfc/nfc.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/iso14443_4a/iso14443_4a_poller.h>

#define TAG "FidoNfcScan"

/* SELECT by name (P1=0x04), the FIDO applet AID as data (fixed by the spec,
 * same bytes fido_nfc.c answers SELECT for on the emulation side), Le=0. */
static const uint8_t fido_select_apdu[] =
    {0x00, 0xA4, 0x04, 0x00, 0x08, 0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01, 0x00};

/* NFCCTAP_MSG (CLA=0x80, INS=0x10) carrying the one-byte CTAP2 command
 * "authenticatorGetInfo" (0x04). It takes no parameters. */
static const uint8_t fido_get_info_apdu[] = {0x80, 0x10, 0x00, 0x00, 0x01, 0x04, 0x00};

static const uint8_t fido_get_response_apdu[] = {0x00, 0xC0, 0x00, 0x00, 0x00};

/* One APDU frame, in either direction. Comfortably above any single GetInfo
 * chunk; chaining (61xx / GET RESPONSE) is what handles the rest. */
#define APDU_FRAME_MAX 256

/* GetInfo responses with several extensions/algorithms can run past one
 * frame; this is generous enough for anything this parser reads out of one. */
#define APDU_TOTAL_MAX 768

struct FidoNfcScan {
    Nfc* nfc;
    NfcPoller* poller;
    FuriMutex* mutex;
    FidoNfcScanResult result;
};

/* -------------------------------------------------------------- transport */

/* Send one APDU, following 61xx (SW_BYTES_REMAINING) with GET RESPONSE until
 * the key hands over a final status word. `out` collects the response body
 * with the trailing SW1SW2 stripped; *out_len is capacity in, bytes out. */
static Iso14443_4aError fido_nfc_scan_exchange(
    Iso14443_4aPoller* poller,
    const uint8_t* apdu,
    size_t apdu_len,
    uint8_t* out,
    size_t* out_len,
    uint16_t* sw) {
    BitBuffer* tx = bit_buffer_alloc(APDU_FRAME_MAX);
    BitBuffer* rx = bit_buffer_alloc(APDU_FRAME_MAX);
    const size_t cap = *out_len;
    size_t total = 0;
    *sw = 0;

    bit_buffer_copy_bytes(tx, apdu, apdu_len);
    Iso14443_4aError err = iso14443_4a_poller_send_block(poller, tx, rx);

    while(err == Iso14443_4aErrorNone) {
        size_t rx_len = bit_buffer_get_size_bytes(rx);
        if(rx_len < 2) {
            err = Iso14443_4aErrorProtocol;
            break;
        }
        const uint8_t* rx_data = bit_buffer_get_data(rx);
        size_t data_len = rx_len - 2;

        size_t room = (cap > total) ? cap - total : 0;
        if(data_len > room) data_len = room;
        memcpy(out + total, rx_data, data_len);
        total += data_len;

        *sw = ((uint16_t)rx_data[rx_len - 2] << 8) | rx_data[rx_len - 1];
        if((*sw & 0xFF00) != 0x6100) break; /* anything but "bytes remaining" ends it */

        bit_buffer_reset(tx);
        bit_buffer_reset(rx);
        bit_buffer_copy_bytes(tx, fido_get_response_apdu, sizeof(fido_get_response_apdu));
        err = iso14443_4a_poller_send_block(poller, tx, rx);
    }

    bit_buffer_free(tx);
    bit_buffer_free(rx);
    *out_len = total;
    return err;
}

/* --------------------------------------------------------------- GetInfo */

static bool fido_nfc_scan_read_options(CborReader* r, FidoNfcScanResult* out) {
    size_t fields = 0;
    if(!cbor_r_map(r, &fields)) return false;

    for(size_t i = 0; i < fields; i++) {
        const char* name = NULL;
        size_t name_len = 0;
        if(!cbor_r_tstr(r, &name, &name_len)) return false;

        if(cbor_r_tstr_equals(name, name_len, "rk")) {
            bool v = false;
            if(!cbor_r_bool(r, &v)) return false;
            out->resident_key = v;
        } else if(cbor_r_tstr_equals(name, name_len, "clientPin")) {
            bool v = false;
            if(!cbor_r_bool(r, &v)) return false;
            out->pin_capable = true;
            out->pin_set = v;
        } else if(!cbor_r_skip(r)) {
            return false;
        }
    }
    return true;
}

/* `payload` is the GetInfo response with the trailing SW already stripped:
 * one CTAP2 status byte, then the CBOR map. */
static void fido_nfc_scan_parse_get_info(const uint8_t* payload, size_t len, FidoNfcScanResult* out) {
    if(len < 1 || payload[0] != 0x00) return; /* the key refused the command */

    CborReader r;
    cbor_r_init(&r, payload + 1, len - 1);

    size_t entries = 0;
    if(!cbor_r_map(&r, &entries)) return;

    for(size_t i = 0; i < entries; i++) {
        uint64_t key = 0;
        if(!cbor_r_uint(&r, &key)) return;

        bool ok = true;
        switch(key) {
        case 1: { /* versions */
            size_t count = 0;
            ok = cbor_r_array(&r, &count);
            for(size_t v = 0; ok && v < count; v++) {
                const char* s = NULL;
                size_t s_len = 0;
                ok = cbor_r_tstr(&r, &s, &s_len);
                if(!ok) break;
                if(cbor_r_tstr_equals(s, s_len, "FIDO_2_0")) out->has_ctap2 = true;
                if(cbor_r_tstr_equals(s, s_len, "U2F_V2")) out->has_u2f = true;
            }
            break;
        }
        case 3: { /* aaguid */
            const uint8_t* data = NULL;
            size_t data_len = 0;
            ok = cbor_r_bstr(&r, &data, &data_len);
            if(ok && data_len == FIDO_NFC_SCAN_AAGUID_SIZE) {
                memcpy(out->aaguid, data, FIDO_NFC_SCAN_AAGUID_SIZE);
                out->has_aaguid = true;
            }
            break;
        }
        case 4: /* options */
            ok = fido_nfc_scan_read_options(&r, out);
            break;
        default:
            ok = cbor_r_skip(&r);
            break;
        }

        if(!ok) return;
    }
}

/* ---------------------------------------------------------------- poller */

static NfcCommand fido_nfc_scan_poller_callback(NfcGenericEvent event, void* context) {
    FidoNfcScan* instance = context;
    const Iso14443_4aPollerEvent* iso_event = event.event_data;

    if(iso_event->type != Iso14443_4aPollerEventTypeReady) {
        /* Activation failed -- not a FIDO applet question at all, just no
         * ISO14443-4 card in range (or one that doesn't support it). Still
         * has to be reported, or the scene is left showing "searching"
         * forever with no poller actually running behind it. */
        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        instance->result.state = FidoNfcScanStateNotFido;
        furi_mutex_release(instance->mutex);
        return NfcCommandStop;
    }

    FidoNfcScanResult result;
    memset(&result, 0, sizeof(result));
    result.state = FidoNfcScanStateNotFido;

    uint8_t buf[APDU_TOTAL_MAX];
    size_t len;
    uint16_t sw;

    len = sizeof(buf);
    Iso14443_4aError err = fido_nfc_scan_exchange(
        event.instance, fido_select_apdu, sizeof(fido_select_apdu), buf, &len, &sw);

    if(err == Iso14443_4aErrorNone && sw == 0x9000) {
        /* SELECT succeeded: this is a FIDO applet even if it turns out to be
         * CTAP1-only, so the state moves off NotFido right away. */
        FURI_LOG_I(TAG, "FIDO applet selected");
        result.state = FidoNfcScanStateDone;
        result.has_u2f = true;

        len = sizeof(buf);
        err = fido_nfc_scan_exchange(
            event.instance, fido_get_info_apdu, sizeof(fido_get_info_apdu), buf, &len, &sw);

        if(err == Iso14443_4aErrorNone && sw == 0x9000) {
            fido_nfc_scan_parse_get_info(buf, len, &result);
        }
        /* Anything else (INS/CLA not supported, wrong length, ...) just means
         * this key has no CTAP2 -- has_u2f alone already says that. */
    }

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->result = result;
    furi_mutex_release(instance->mutex);

    return NfcCommandStop;
}

/* ------------------------------------------------------------- lifecycle */

FidoNfcScan* fido_nfc_scan_start(void) {
    FidoNfcScan* instance = calloc(1, sizeof(FidoNfcScan));
    instance->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->result.state = FidoNfcScanStateSearching;

    instance->nfc = nfc_alloc();
    instance->poller = nfc_poller_alloc(instance->nfc, NfcProtocolIso14443_4a);
    nfc_poller_start(instance->poller, fido_nfc_scan_poller_callback, instance);

    return instance;
}

void fido_nfc_scan_stop(FidoNfcScan* instance) {
    furi_assert(instance);

    nfc_poller_stop(instance->poller);
    nfc_poller_free(instance->poller);
    nfc_free(instance->nfc);

    furi_mutex_free(instance->mutex);
    memset(instance, 0, sizeof(FidoNfcScan));
    free(instance);
}

void fido_nfc_scan_get_result(FidoNfcScan* instance, FidoNfcScanResult* out) {
    furi_assert(instance);
    furi_assert(out);

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    *out = instance->result;
    furi_mutex_release(instance->mutex);
}
