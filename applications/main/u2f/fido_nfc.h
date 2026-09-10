#pragma once

/* FIDO over NFC -- the transport that actually gets this board off the cable.
 *
 * A phone talks to the authenticator as an ISO14443-4 (ISO-DEP) smartcard: it
 * selects the FIDO applet by AID, then sends CTAP1 or CTAP2 messages wrapped in
 * ISO7816 APDUs. The PN532 handles activation, RATS/ATS and ISO-DEP framing in
 * hardware, so what is left here is the applet itself.
 *
 * The awkward part is length. The PN532 hands up at most ~262 bytes per frame,
 * so extended-length APDUs are not usable in either direction and everything
 * has to go through short-APDU chaining: CLA bit 0x10 for a request that does
 * not fit, and 61xx plus GET RESPONSE for a response that does not. A CTAP2
 * makeCredential response is around 670 bytes, so this is not optional.
 *
 * User presence is the tag being in the field, per the FIDO NFC spec -- there
 * is no way to press a button while a phone is held against the antenna, and
 * removing the phone is itself the cancel gesture.
 */

#include "u2f.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FidoNfc FidoNfc;

/** Start card emulation on its own thread. Borrows `u2f` -- the device key,
 * counter and credentials are shared with the USB transport, which is what
 * makes a credential registered over USB usable over NFC. */
FidoNfc* fido_nfc_start(U2fData* u2f);

void fido_nfc_stop(FidoNfc* instance);

typedef enum {
    FidoNfcHalStarting, /* the worker has not reached the radio yet */
    FidoNfcHalReady,    /* the applet is listening */
    FidoNfcHalFailed,   /* the NFC HAL could not be acquired */
} FidoNfcHalState;

/* Enough to tell the three silent failures apart when a tap does nothing:
 * the radio never came up, the reader never coupled, or it coupled and never
 * asked for FIDO. Without this they all look like a blank screen. */
typedef struct {
    FidoNfcHalState hal;
    /* A reader has exchanged at least one APDU with the emulated card. */
    bool activated;
    /* A reader has selected the FIDO applet by AID -- the difference between
     * "something scanned the card" and "something is speaking FIDO". Sticky,
     * so a deselect at the end of a ceremony does not erase the evidence. */
    bool selected;
    uint32_t apdu_count;
} FidoNfcStatus;

/** Snapshot of what the NFC side has seen, for the on-screen status. Safe to
 * call from another thread; the fields are written by the worker and read
 * here without locking, which is fine for a display. */
void fido_nfc_get_status(const FidoNfc* instance, FidoNfcStatus* out);

#ifdef __cplusplus
}
#endif
