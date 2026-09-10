#pragma once

/* Resident (discoverable) credentials -- WebAuthn "passkeys".
 *
 * A non-resident credential is wrapped into its own credential id and stored
 * nowhere; the relying party hands it back in allowList and the device
 * unwraps it. A discoverable one must be findable from the rpId ALONE, because
 * the site asks "who do you have for me?" before knowing any credential id.
 * That means the device has to keep a record.
 *
 * What is kept is deliberately not the private key: credential ids here use
 * the same wrapped nonce+MAC construction as everywhere else, so the key is
 * re-derived from the device key on use. The record holds only what cannot be
 * derived -- which user this is, and for which site. One consequence worth
 * knowing: regenerating the device key (authenticatorReset) invalidates these
 * too, exactly as it does non-resident credentials.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctap2_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The card would hold far more; this bounds the enumeration a getAssertion has
 * to do, and getInfo reports it so clients do not over-commit. */
#define CTAP2_RK_MAX_CREDENTIALS 32

#define CTAP2_RK_USER_ID_MAX 64
#define CTAP2_RK_NAME_MAX    32 /* plus a NUL */

typedef struct {
    uint8_t rp_id_hash[CTAP2_RPID_HASH_SIZE];
    uint8_t cred_id[CTAP2_CRED_ID_SIZE];
    uint8_t user_id[CTAP2_RK_USER_ID_MAX];
    uint8_t user_id_len;
    char user_name[CTAP2_RK_NAME_MAX + 1];
    char display_name[CTAP2_RK_NAME_MAX + 1];
    char rp_id[CTAP2_RK_NAME_MAX + 1];
} Ctap2RkRecord;

/** Write a record, replacing any existing credential for the same (rp, user).
 * Replacing rather than accumulating is what the spec requires: registering
 * again at a site you already use must not leave the old passkey behind. */
bool ctap2_rk_store(const Ctap2RkRecord* record);

/** How many discoverable credentials exist for this rp. */
size_t ctap2_rk_count_for_rp(const uint8_t* rp_id_hash);

/** Fetch the `index`-th credential for this rp (0-based, in directory order).
 *
 * Deliberately re-enumerates per call rather than returning the whole list:
 * one record is 272 bytes, and holding 32 of them would cost more RAM than the
 * whole CTAP2 layer uses. getNextAssertion only ever needs one at a time. */
bool ctap2_rk_get_for_rp(const uint8_t* rp_id_hash, size_t index, Ctap2RkRecord* out);

/** Fetch the `index`-th credential across ALL sites, for the on-device list.
 * Directory order, which is stable between calls as long as nothing is
 * written. */
bool ctap2_rk_get_any(size_t index, Ctap2RkRecord* out);

/** Whether a credential already exists for this exact (rp, user).
 *
 * The distinction matters only when the store is full: re-registering an
 * account that is already here replaces its record and needs no new room,
 * whereas a genuinely new one has nowhere to go. */
bool ctap2_rk_exists(const uint8_t* rp_id_hash, const uint8_t* user_id, size_t user_id_len);

/** Delete the credential for this exact (rp, user).
 *
 * Same deterministic filename as ctap2_rk_store, so this removes exactly the
 * record the on-device list showed. True when the file is gone afterwards,
 * including when it was already absent. */
bool ctap2_rk_delete(const uint8_t* rp_id_hash, const uint8_t* user_id, size_t user_id_len);

/** Total across all sites, for the storage-full check. */
size_t ctap2_rk_count_all(void);

/** Delete every stored credential. Part of authenticatorReset. */
void ctap2_rk_wipe(void);

#ifdef __cplusplus
}
#endif
