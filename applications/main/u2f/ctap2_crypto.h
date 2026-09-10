#pragma once

/* CTAP2 credential derivation, authenticatorData assembly and COSE encoding.
 *
 * Split out from ctap2.c because ctap2_rk.c (resident credentials) needs the
 * same authData and COSE builders, and because these are the parts worth
 * reading on their own when a signature does not verify.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fido_cbor.h"
#include "u2f_ecc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CTAP2_AAGUID_SIZE   16
#define CTAP2_RPID_HASH_SIZE 32
#define CTAP2_CRED_ID_SIZE  64 /* nonce ‖ mac */
#define CTAP2_NONCE_SIZE    32
#define CTAP2_MAC_SIZE      32

/* authenticatorData flags. */
#define CTAP2_FLAG_UP 0x01 /* user present  */
#define CTAP2_FLAG_UV 0x04 /* user verified */
#define CTAP2_FLAG_AT 0x40 /* attested credential data included */
#define CTAP2_FLAG_ED 0x80 /* extension data included */

/** This authenticator's model identifier. Chosen once and **never changed** --
 * relying parties key attestation metadata and policy on it, so a new value
 * makes the device look like different hardware.
 * 83b92959-e480-473a-9a77-8f5429afb5ce */
extern const uint8_t ctap2_aaguid[CTAP2_AAGUID_SIZE];

/** Derive the per-credential private key that credential id `nonce` denotes.
 *
 * Identical in construction to the U2F key handle scheme in u2f.c, which is
 * deliberate: both protocols wrap a key under the same device key rather than
 * storing it, so a non-resident CTAP2 credential costs no flash and survives a
 * card wipe exactly as a U2F registration does. */
void ctap2_derive_private_key(
    const uint8_t* device_key,
    const uint8_t* rp_id_hash,
    const uint8_t* nonce,
    uint8_t* private_key_out);

/** Build a credential id for a freshly generated credential: a random nonce
 * plus a MAC binding it to this rp and this device key. */
void ctap2_make_credential_id(
    const uint8_t* device_key,
    const uint8_t* rp_id_hash,
    uint8_t* cred_id_out,
    uint8_t* private_key_out);

/** Recover the private key from a credential id, rejecting one that was not
 * issued by this device for this rp. Constant-time MAC comparison. */
bool ctap2_open_credential_id(
    const uint8_t* device_key,
    const uint8_t* rp_id_hash,
    const uint8_t* cred_id,
    size_t cred_id_len,
    uint8_t* private_key_out);

/** Encode an ES256 public key as a COSE_Key map, in CTAP2 canonical key order.
 * Emitted straight into the caller's CBOR writer. */
void ctap2_write_cose_key(CborWriter* w, const U2fPubKey* pub_key);

/** Assemble authenticatorData.
 *
 * `pub_key` and `cred_id` are supplied together for a makeCredential (which
 * sets the AT flag and appends attested credential data) and both NULL for a
 * getAssertion. Returns the number of bytes written, or 0 if `cap` is too
 * small -- a truncated authData would produce a signature over the wrong
 * bytes, so callers must treat 0 as fatal rather than continuing. */
size_t ctap2_build_auth_data(
    uint8_t* out,
    size_t cap,
    const uint8_t* rp_id_hash,
    uint8_t flags,
    uint32_t sign_count,
    const uint8_t* cred_id,
    size_t cred_id_len,
    const U2fPubKey* pub_key);

#ifdef __cplusplus
}
#endif
