#pragma once

/* CTAP2 clientPIN, PIN/UV auth protocol version 1.
 *
 * Two entry points into the same stored PIN:
 *
 *  - the host, via authenticatorClientPIN (0x06), which is the standard flow
 *    Windows and Chrome drive; and
 *  - the board's own Settings screen, which sets or changes the PIN locally.
 *
 * Both are legitimate. The authenticator only ever stores SHA-256(pin)[0..15],
 * so it cannot tell -- and does not care -- whether the string arrived over an
 * ECDH-encrypted setPIN or off the scroll wheel. Whichever route set it, the
 * user types the same PIN when a host prompts and the hashes match.
 *
 * Protocol v2 is deliberately not implemented: it needs HKDF (currently off in
 * sdkconfig) and every client this device will meet supports v1.
 *
 * The stored-PIN functions take no instance and re-read the file on each call.
 * That is what lets the GUI thread (Settings) and the transport worker touch
 * the same PIN without sharing mutable state between them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <mbedtls/ecp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A CTAP2 PIN is 4..63 UTF-8 BYTES. The upper bound here is this device's own
 * limit on locally-entered PINs, not the protocol's -- a host may legitimately
 * set a longer one, and CTAP2_PIN_MAX_LEN bounds what we accept from either. */
#define CTAP2_PIN_MIN_LEN       4
#define CTAP2_PIN_MAX_LEN       63
#define CTAP2_PIN_LOCAL_MAX_LEN 8 /* digits enterable on the wheel */

#define CTAP2_PIN_HASH_SIZE  16
#define CTAP2_PIN_TOKEN_SIZE 32

/* Total attempts before the PIN is dead and only a reset revives it, and how
 * many may be spent in a single boot before a power cycle is required. Both
 * are the values the spec mandates. */
#define CTAP2_PIN_MAX_RETRIES      8
#define CTAP2_PIN_RETRIES_PER_BOOT 3

typedef struct Ctap2Pin Ctap2Pin;

/* ------------------------------------------------- stored PIN (no instance) */

/** True when a PIN has been set. Drives getInfo's options.clientPin, so a
 * client knows whether to offer "set a PIN" or "enter your PIN". */
bool ctap2_pin_is_set(void);

/** Attempts left before the PIN is permanently blocked, or
 * CTAP2_PIN_MAX_RETRIES when none is set. */
uint8_t ctap2_pin_get_retries(void);

/** Store a new PIN and restore the retry counter. Rejects anything shorter
 * than CTAP2_PIN_MIN_LEN. */
bool ctap2_pin_store(const uint8_t* pin, size_t pin_len);

/** Compare against the stored PIN hash, spending a retry on failure and
 * restoring the counter on success. Returns false when blocked. */
bool ctap2_pin_check_hash(const uint8_t* pin_hash);

/** Same, for a PIN entered on the device. */
bool ctap2_pin_check(const uint8_t* pin, size_t pin_len);

/** Remove the PIN entirely. The caller must have verified it first -- this
 * does not check, because authenticatorReset legitimately wipes without one. */
bool ctap2_pin_erase(void);

/* ------------------------------------------------------------- protocol */

Ctap2Pin* ctap2_pin_alloc(void);
void ctap2_pin_free(Ctap2Pin* instance);

/** Handle authenticatorClientPIN. `req` is the CBOR body, without the command
 * byte; `resp[0]` receives the status. Returns the total response length. */
size_t ctap2_pin_command(
    Ctap2Pin* instance,
    mbedtls_ecp_group* group,
    const uint8_t* req,
    size_t req_len,
    uint8_t* resp,
    size_t resp_cap);

/** Verify a pinUvAuthParam accompanying makeCredential or getAssertion:
 * LEFT(HMAC-SHA256(pinToken, data), 16). A true result is what sets the UV
 * flag in authenticatorData, so this is the whole basis of the "user verified"
 * claim a relying party receives. */
bool ctap2_pin_verify_auth_param(
    Ctap2Pin* instance,
    const uint8_t* data,
    size_t data_len,
    const uint8_t* param,
    size_t param_len);

/** Invalidate the issued pinToken. Called after setPIN/changePIN and on reset,
 * so a token minted under the old PIN cannot outlive it. */
void ctap2_pin_revoke_token(Ctap2Pin* instance);

#ifdef __cplusplus
}
#endif
