#include "ctap2_crypto.h"

#include <string.h>

#include <furi.h>
#include <furi_hal_random.h>

#include <mbedtls/md.h>

#define TAG "Ctap2Crypto"

#define MCHECK(expr) furi_check((expr) == 0)

/* COSE labels and values, from RFC 8152 / the WebAuthn registry. */
#define COSE_KEY_KTY    1
#define COSE_KEY_ALG    3
#define COSE_KEY_CRV    (-1)
#define COSE_KEY_X      (-2)
#define COSE_KEY_Y      (-3)
#define COSE_KTY_EC2    2
#define COSE_ALG_ES256  (-7)
#define COSE_CRV_P256   1

const uint8_t ctap2_aaguid[CTAP2_AAGUID_SIZE] = {
    0x83,
    0xB9,
    0x29,
    0x59,
    0xE4,
    0x80,
    0x47,
    0x3A,
    0x9A,
    0x77,
    0x8F,
    0x54,
    0x29,
    0xAF,
    0xB5,
    0xCE,
};

/* One HMAC-SHA256 over two chunks. Both derivations below are that shape, and
 * routing them through a single helper keeps the key material out of the
 * call sites entirely. */
static void ctap2_hmac2(
    const uint8_t* key,
    size_t key_len,
    const uint8_t* a,
    size_t a_len,
    const uint8_t* b,
    size_t b_len,
    uint8_t* out) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    MCHECK(mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1));
    MCHECK(mbedtls_md_hmac_starts(&ctx, key, key_len));
    MCHECK(mbedtls_md_hmac_update(&ctx, a, a_len));
    MCHECK(mbedtls_md_hmac_update(&ctx, b, b_len));
    MCHECK(mbedtls_md_hmac_finish(&ctx, out));
    mbedtls_md_free(&ctx);
}

void ctap2_derive_private_key(
    const uint8_t* device_key,
    const uint8_t* rp_id_hash,
    const uint8_t* nonce,
    uint8_t* private_key_out) {
    ctap2_hmac2(
        device_key,
        U2F_EC_KEY_SIZE,
        rp_id_hash,
        CTAP2_RPID_HASH_SIZE,
        nonce,
        CTAP2_NONCE_SIZE,
        private_key_out);
}

/* The MAC binds the derived key to this rp, so a credential id lifted from one
 * site cannot be replayed at another. */
static void ctap2_derive_mac(
    const uint8_t* device_key,
    const uint8_t* rp_id_hash,
    const uint8_t* private_key,
    uint8_t* mac_out) {
    ctap2_hmac2(
        device_key,
        U2F_EC_KEY_SIZE,
        private_key,
        U2F_EC_KEY_SIZE,
        rp_id_hash,
        CTAP2_RPID_HASH_SIZE,
        mac_out);
}

void ctap2_make_credential_id(
    const uint8_t* device_key,
    const uint8_t* rp_id_hash,
    uint8_t* cred_id_out,
    uint8_t* private_key_out) {
    furi_hal_random_fill_buf(cred_id_out, CTAP2_NONCE_SIZE);
    ctap2_derive_private_key(device_key, rp_id_hash, cred_id_out, private_key_out);
    ctap2_derive_mac(device_key, rp_id_hash, private_key_out, cred_id_out + CTAP2_NONCE_SIZE);
}

bool ctap2_open_credential_id(
    const uint8_t* device_key,
    const uint8_t* rp_id_hash,
    const uint8_t* cred_id,
    size_t cred_id_len,
    uint8_t* private_key_out) {
    if(cred_id_len != CTAP2_CRED_ID_SIZE) return false;

    uint8_t expected_mac[CTAP2_MAC_SIZE];
    ctap2_derive_private_key(device_key, rp_id_hash, cred_id, private_key_out);
    ctap2_derive_mac(device_key, rp_id_hash, private_key_out, expected_mac);

    /* Constant time: this compares a value an attacker supplies against one
     * derived from the device key, and a byte-at-a-time memcmp would leak how
     * much of a forged MAC was right. */
    uint8_t diff = 0;
    for(size_t i = 0; i < CTAP2_MAC_SIZE; i++) {
        diff |= (uint8_t)(cred_id[CTAP2_NONCE_SIZE + i] ^ expected_mac[i]);
    }

    if(diff != 0) {
        memset(private_key_out, 0, U2F_EC_KEY_SIZE);
        return false;
    }
    return true;
}

void ctap2_write_cose_key(CborWriter* w, const U2fPubKey* pub_key) {
    /* CTAP2 canonical CBOR sorts map keys by encoded length then bytewise.
     * All five labels encode to one byte -- 0x01, 0x03, 0x20, 0x21, 0x22 --
     * so this order is the canonical one and must not be rearranged. */
    cbor_w_map(w, 5);

    cbor_w_int(w, COSE_KEY_KTY);
    cbor_w_int(w, COSE_KTY_EC2);

    cbor_w_int(w, COSE_KEY_ALG);
    cbor_w_int(w, COSE_ALG_ES256);

    cbor_w_int(w, COSE_KEY_CRV);
    cbor_w_int(w, COSE_CRV_P256);

    /* pub_key->xy is the uncompressed point body: X then Y, 32 bytes each.
     * The leading 0x04 format byte is not part of the COSE encoding. */
    cbor_w_int(w, COSE_KEY_X);
    cbor_w_bstr(w, pub_key->xy, 32);

    cbor_w_int(w, COSE_KEY_Y);
    cbor_w_bstr(w, pub_key->xy + 32, 32);
}

size_t ctap2_build_auth_data(
    uint8_t* out,
    size_t cap,
    const uint8_t* rp_id_hash,
    uint8_t flags,
    uint32_t sign_count,
    const uint8_t* cred_id,
    size_t cred_id_len,
    const U2fPubKey* pub_key) {
    const bool attested = (cred_id != NULL && pub_key != NULL);
    size_t len = 0;

    /* Fixed part: rpIdHash ‖ flags ‖ signCount. */
    if(cap < CTAP2_RPID_HASH_SIZE + 1 + 4) return 0;

    memcpy(out, rp_id_hash, CTAP2_RPID_HASH_SIZE);
    len += CTAP2_RPID_HASH_SIZE;

    out[len++] = (uint8_t)(attested ? (flags | CTAP2_FLAG_AT) : flags);

    /* signCount is big-endian on the wire. */
    out[len++] = (uint8_t)(sign_count >> 24);
    out[len++] = (uint8_t)(sign_count >> 16);
    out[len++] = (uint8_t)(sign_count >> 8);
    out[len++] = (uint8_t)sign_count;

    if(!attested) return len;

    if(cap - len < CTAP2_AAGUID_SIZE + 2 + cred_id_len) return 0;

    /* Zero, not ctap2_aaguid: CTAP2 requires an authenticator using SELF
     * attestation to report an all-zero AAGUID in attested credential data,
     * and strict verifiers check it. getInfo still reports the real one --
     * that is the model identifier, and it is not what correlates a user.
     *
     * It also removes the "unrecognised authenticator <uuid>" a relying party
     * shows for an AAGUID absent from the FIDO metadata service, which this
     * one will always be. */
    memset(out + len, 0, CTAP2_AAGUID_SIZE);
    len += CTAP2_AAGUID_SIZE;

    out[len++] = (uint8_t)(cred_id_len >> 8);
    out[len++] = (uint8_t)cred_id_len;

    memcpy(out + len, cred_id, cred_id_len);
    len += cred_id_len;

    /* The COSE key is CBOR, so it is encoded straight into the tail of the
     * buffer rather than built elsewhere and copied. */
    CborWriter w;
    cbor_w_init(&w, out + len, cap - len);
    ctap2_write_cose_key(&w, pub_key);

    size_t cose_len = cbor_w_finish(&w);
    if(cose_len == 0) {
        FURI_LOG_E(TAG, "authData buffer too small for COSE key");
        return 0;
    }

    return len + cose_len;
}
