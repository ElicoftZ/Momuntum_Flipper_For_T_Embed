#pragma once

/* Inspecting SOMEONE ELSE'S security key -- the reverse of fido_nfc.c.
 *
 * There this board is the tag and a phone or computer is the reader; here the
 * Flipper is the reader (an ISO14443-4A poller) and the thing held to the
 * antenna is a NFC-capable FIDO2/U2F authenticator. Everything read here is
 * exactly what that key volunteers, unauthenticated, to any reader that asks:
 * SELECT the FIDO applet by AID, then send authenticatorGetInfo. No PIN, no
 * credential, no private material is or can be touched by this -- GetInfo is
 * capability advertisement, the same handshake a browser does before it even
 * shows the user a prompt.
 *
 * A key that only speaks CTAP1/U2F has no GetInfo at all -- there is no
 * clientPin concept in that protocol -- so "PIN protected" is only ever
 * answerable for a CTAP2 key, and is reported as "unknown" otherwise rather
 * than guessed.
 *
 * USB-only keys (most YubiKeys without an NFC model) are out of reach: this
 * firmware's USB stack is device-mode only, so there is no way for the board
 * to act as a host and query one over USB.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FidoNfcScan FidoNfcScan;

typedef enum {
    FidoNfcScanStateSearching, /* poller running, nothing has answered yet */
    FidoNfcScanStateNotFido, /* a tag answered, but not the FIDO applet */
    FidoNfcScanStateDone, /* the FIDO applet answered -- see FidoNfcScanResult */
    FidoNfcScanStateError, /* the NFC radio itself could not be started */
} FidoNfcScanState;

#define FIDO_NFC_SCAN_AAGUID_SIZE 16

typedef struct {
    FidoNfcScanState state;

    /* From the SELECT response and, when present, GetInfo's versions list. */
    bool has_u2f; /* speaks CTAP1 (U2F_V2) */
    bool has_ctap2; /* answered authenticatorGetInfo -- speaks CTAP2 (FIDO2) */

    /* Only meaningful when has_ctap2: U2F alone carries no PIN concept, so a
     * CTAP1-only key leaves both of these false rather than guessed. */
    bool pin_capable; /* advertises the clientPin option at all */
    bool pin_set; /* clientPin == true: a PIN has actually been configured */

    bool resident_key; /* rk option: stores discoverable credentials/passkeys */

    bool has_aaguid;
    uint8_t aaguid[FIDO_NFC_SCAN_AAGUID_SIZE];
} FidoNfcScanResult;

/** Start polling for a nearby FIDO2/U2F security key. Runs on the NFC
 * poller's own worker; nothing here blocks the caller. */
FidoNfcScan* fido_nfc_scan_start(void);

void fido_nfc_scan_stop(FidoNfcScan* instance);

/** Snapshot of what has been read so far. Safe to call from another thread
 * while a scan is running. */
void fido_nfc_scan_get_result(FidoNfcScan* instance, FidoNfcScanResult* out);

#ifdef __cplusplus
}
#endif
