#pragma once

/* CTAP2 / FIDO2 command layer.
 *
 * Transport-agnostic on purpose: USB HID, NFC and BLE all hand the same CBOR
 * message in here and get one back. The only thing a transport has to supply
 * beyond the bytes is how to obtain user presence, because that differs
 * fundamentally between them -- USB streams KEEPALIVEs while it waits for the
 * OK press, whereas over NFC the spec treats the tag being on the reader AS
 * the user's presence and there is no button to press anyway.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "u2f.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Advertised as maxMsgSize in getInfo, and the size a transport must be able
 * to carry in one CTAP2 message. */
#define CTAP2_MAX_MSG_SIZE 1200

/* Commands (CTAP 2.1 §6). */
#define CTAP2_CMD_MAKE_CREDENTIAL   0x01
#define CTAP2_CMD_GET_ASSERTION     0x02
#define CTAP2_CMD_GET_INFO          0x04
#define CTAP2_CMD_CLIENT_PIN        0x06
#define CTAP2_CMD_RESET             0x07
#define CTAP2_CMD_GET_NEXT_ASSERTION 0x08

/* Status codes (CTAP 2.1 §6.3). Only the ones this firmware can return. */
#define CTAP2_OK                        0x00
#define CTAP1_ERR_INVALID_COMMAND       0x01
#define CTAP1_ERR_INVALID_PARAMETER     0x02
#define CTAP1_ERR_INVALID_LENGTH        0x03
#define CTAP2_ERR_CBOR_UNEXPECTED_TYPE  0x11
#define CTAP2_ERR_INVALID_CBOR          0x12
#define CTAP2_ERR_MISSING_PARAMETER     0x14
#define CTAP2_ERR_LIMIT_EXCEEDED        0x15
#define CTAP2_ERR_CREDENTIAL_EXCLUDED   0x19
#define CTAP2_ERR_PROCESSING            0x21
#define CTAP2_ERR_INVALID_CREDENTIAL    0x22
#define CTAP2_ERR_UNSUPPORTED_ALGORITHM 0x26
#define CTAP2_ERR_OPERATION_DENIED      0x27
#define CTAP2_ERR_KEY_STORE_FULL        0x28
#define CTAP2_ERR_UNSUPPORTED_OPTION    0x2B
#define CTAP2_ERR_INVALID_OPTION        0x2C
#define CTAP2_ERR_KEEPALIVE_CANCEL      0x2D
#define CTAP2_ERR_NO_CREDENTIALS        0x2E
#define CTAP2_ERR_USER_ACTION_TIMEOUT   0x2F
#define CTAP2_ERR_NOT_ALLOWED           0x30
#define CTAP2_ERR_PIN_INVALID           0x31
#define CTAP2_ERR_PIN_BLOCKED           0x32
#define CTAP2_ERR_PIN_AUTH_INVALID      0x33
#define CTAP2_ERR_PIN_AUTH_BLOCKED      0x34
#define CTAP2_ERR_PIN_NOT_SET           0x35
#define CTAP2_ERR_PIN_REQUIRED          0x36
#define CTAP2_ERR_PIN_POLICY_VIOLATION  0x37
#define CTAP2_ERR_REQUEST_TOO_LARGE     0x39
#define CTAP2_ERR_UP_REQUIRED           0x3B
#define CTAP1_ERR_OTHER                 0x7F

typedef struct Ctap2 Ctap2;

typedef enum {
    Ctap2PresenceGranted,
    Ctap2PresenceTimeout,
    Ctap2PresenceCancelled, /* the host sent CTAPHID_CANCEL */
} Ctap2PresenceResult;

/** Obtain user presence. `prompt` is U2fNotifyRegister or U2fNotifyAuth so the
 * transport can put the right words on the screen. Must block until the user
 * acts, the host cancels, or the transport's own timeout expires. */
typedef Ctap2PresenceResult (*Ctap2PresenceFn)(void* context, U2fNotifyEvent prompt);

/** Borrows `u2f` -- device key, curve parameters and the signature counter all
 * stay owned by it, so the two protocols cannot drift apart. Does not take
 * ownership; free this before the U2fData it was built from. */
Ctap2* ctap2_alloc(U2fData* u2f);

void ctap2_free(Ctap2* instance);

void ctap2_set_presence_callback(Ctap2* instance, Ctap2PresenceFn callback, void* context);

/** Handle one CTAP2 message. `req[0]` is the command byte, the rest is CBOR.
 * Writes `resp[0]` = status followed by the CBOR response, and returns the
 * total length -- always at least 1, since even a failure returns its status.
 *
 * `req` and `resp` must not overlap: the CBOR reader hands out pointers into
 * `req` that stay live while the response is being written. */
size_t ctap2_request(
    Ctap2* instance,
    const uint8_t* req,
    size_t req_len,
    uint8_t* resp,
    size_t resp_cap);

/** Last command byte and status seen, for the on-screen diagnostics. There is
 * no serial console while the USB composite owns the PHY, so this is how a
 * failing ceremony gets diagnosed at all. */
uint8_t ctap2_last_command(const Ctap2* instance);
uint8_t ctap2_last_status(const Ctap2* instance);

#ifdef __cplusplus
}
#endif
