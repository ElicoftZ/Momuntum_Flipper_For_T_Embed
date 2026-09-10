#include "ctap2_pin.h"

#include "ctap2.h"
#include "fido_cbor.h"
#include "u2f_ecc.h"

#include <string.h>

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_random.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#include <mbedtls/aes.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#define TAG "Ctap2Pin"

#define MCHECK(expr) furi_check((expr) == 0)

#define U2F_PIN_FILE EXT_PATH("u2f/pin.u2f")

#define U2F_PIN_FILE_TYPE "Flipper U2F PIN File"
#define U2F_PIN_VERSION   1

/* Sealed with the same device-unique enclave slot as every other u2f file. */
#define U2F_PIN_KEY_SLOT FURI_HAL_CRYPTO_ENCLAVE_UNIQUE_KEY_SLOT

/* Distinguishes a successful decryption from plausible garbage: without it a
 * wrong device key would yield random bytes that look like a valid PIN hash. */
#define U2F_PIN_CONTROL_VAL 0x50494E32UL /* "PIN2" */

/* clientPIN subcommands (CTAP 2.1 §6.5.5). */
#define PIN_SUB_GET_RETRIES     0x01
#define PIN_SUB_GET_KEY_AGREE   0x02
#define PIN_SUB_SET_PIN         0x03
#define PIN_SUB_CHANGE_PIN      0x04
#define PIN_SUB_GET_PIN_TOKEN   0x05

/* clientPIN request map keys. */
#define PIN_KEY_PROTOCOL      1
#define PIN_KEY_SUBCOMMAND    2
#define PIN_KEY_KEY_AGREEMENT 3
#define PIN_KEY_PIN_AUTH      4
#define PIN_KEY_NEW_PIN_ENC   5
#define PIN_KEY_PIN_HASH_ENC  6

/* COSE labels, plus the ECDH algorithm identifier protocol v1 uses. */
#define COSE_KEY_KTY   1
#define COSE_KEY_ALG   3
#define COSE_KEY_CRV   (-1)
#define COSE_KEY_X     (-2)
#define COSE_KEY_Y     (-3)
#define COSE_KTY_EC2   2
#define COSE_CRV_P256  1
#define COSE_ALG_ECDH_ES_HKDF256 (-25)

#define P256_COORD_SIZE 32
#define AES_BLOCK_SIZE  16
#define SHARED_SECRET_SIZE 32

/* The PIN is zero-padded to at least 64 bytes before encryption, so newPinEnc
 * is always at least this long. */
#define PIN_PADDED_LEN 64

/* On-card record. Exactly two AES blocks, so it encrypts without padding. */
typedef struct {
    uint8_t pin_hash[CTAP2_PIN_HASH_SIZE];
    uint8_t retries;
    uint8_t reserved[7];
    uint8_t random_salt[4];
    uint32_t control;
} FURI_PACKED U2fPinData;
_Static_assert(sizeof(U2fPinData) == 32, "U2fPinData must be a whole number of AES blocks");

struct Ctap2Pin {
    /* Ephemeral ECDH keypair, regenerated at every power-on. Its lifetime is
     * what stops a sharedSecret recovered from one session being replayed
     * into the next. */
    mbedtls_mpi agreement_private;
    mbedtls_ecp_point agreement_public;
    bool agreement_ready;

    uint8_t pin_token[CTAP2_PIN_TOKEN_SIZE];
    bool pin_token_valid;

    /* Consecutive failures THIS boot. Separate from the on-card retry counter:
     * the spec wants a power cycle after three wrong guesses even though six
     * of the eight total attempts may remain. */
    uint8_t boot_failures;
};

/* --------------------------------------------------------- stored PIN */

static bool ctap2_pin_load(U2fPinData* out) {
    bool state = false;
    uint8_t iv[16];
    uint8_t encrypted[sizeof(U2fPinData)];
    uint32_t version = 0;

    FuriString* filetype = furi_string_alloc();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);

    if(flipper_format_file_open_existing(ff, U2F_PIN_FILE)) {
        do {
            if(!flipper_format_read_header(ff, filetype, &version)) break;
            if(strcmp(furi_string_get_cstr(filetype), U2F_PIN_FILE_TYPE) != 0 ||
               version != U2F_PIN_VERSION) {
                FURI_LOG_E(TAG, "PIN file type or version mismatch");
                break;
            }
            if(!flipper_format_read_hex(ff, "IV", iv, sizeof(iv))) break;
            if(!flipper_format_read_hex(ff, "Data", encrypted, sizeof(encrypted))) break;

            if(!furi_hal_crypto_enclave_load_key(U2F_PIN_KEY_SLOT, iv)) {
                FURI_LOG_E(TAG, "Unable to load encryption key");
                break;
            }
            memset(out, 0, sizeof(U2fPinData));
            bool decrypted =
                furi_hal_crypto_decrypt(encrypted, (uint8_t*)out, sizeof(U2fPinData));
            furi_hal_crypto_enclave_unload_key(U2F_PIN_KEY_SLOT);

            if(!decrypted) {
                memset(out, 0, sizeof(U2fPinData));
                break;
            }
            if(out->control != U2F_PIN_CONTROL_VAL) {
                /* Wrong device key, or a corrupt file. Either way this is not
                 * our PIN and must not be treated as one. */
                memset(out, 0, sizeof(U2fPinData));
                FURI_LOG_E(TAG, "PIN record failed its control check");
                break;
            }
            state = true;
        } while(0);
    }

    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
    furi_string_free(filetype);
    return state;
}

static bool ctap2_pin_save(U2fPinData* data) {
    bool state = false;
    uint8_t iv[16];
    uint8_t encrypted[sizeof(U2fPinData)];

    furi_hal_random_fill_buf(iv, sizeof(iv));
    furi_hal_random_fill_buf(data->random_salt, sizeof(data->random_salt));
    memset(data->reserved, 0, sizeof(data->reserved));
    data->control = U2F_PIN_CONTROL_VAL;

    if(!furi_hal_crypto_enclave_load_key(U2F_PIN_KEY_SLOT, iv)) {
        FURI_LOG_E(TAG, "Unable to load encryption key");
        return false;
    }
    bool encrypted_ok =
        furi_hal_crypto_encrypt((uint8_t*)data, encrypted, sizeof(U2fPinData));
    furi_hal_crypto_enclave_unload_key(U2F_PIN_KEY_SLOT);
    if(!encrypted_ok) {
        FURI_LOG_E(TAG, "PIN encryption failed");
        return false;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);

    if(flipper_format_file_open_always(ff, U2F_PIN_FILE)) {
        do {
            if(!flipper_format_write_header_cstr(ff, U2F_PIN_FILE_TYPE, U2F_PIN_VERSION)) break;
            if(!flipper_format_write_hex(ff, "IV", iv, sizeof(iv))) break;
            if(!flipper_format_write_hex(ff, "Data", encrypted, sizeof(encrypted))) break;
            state = true;
        } while(0);
    }

    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
    return state;
}

bool ctap2_pin_is_set(void) {
    U2fPinData data;
    bool loaded = ctap2_pin_load(&data);
    memset(&data, 0, sizeof(data));
    return loaded;
}

uint8_t ctap2_pin_get_retries(void) {
    U2fPinData data;
    if(!ctap2_pin_load(&data)) return CTAP2_PIN_MAX_RETRIES;
    uint8_t retries = data.retries;
    memset(&data, 0, sizeof(data));
    return retries;
}

/* SHA-256(pin) truncated to 16 bytes -- the "pinHash" the protocol works in. */
static void ctap2_pin_hash(const uint8_t* pin, size_t pin_len, uint8_t* out) {
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, pin, pin_len);
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    memcpy(out, digest, CTAP2_PIN_HASH_SIZE);
    memset(digest, 0, sizeof(digest));
}

bool ctap2_pin_store(const uint8_t* pin, size_t pin_len) {
    if(pin_len < CTAP2_PIN_MIN_LEN || pin_len > CTAP2_PIN_MAX_LEN) return false;

    U2fPinData data;
    memset(&data, 0, sizeof(data));
    ctap2_pin_hash(pin, pin_len, data.pin_hash);
    data.retries = CTAP2_PIN_MAX_RETRIES;

    bool ok = ctap2_pin_save(&data);
    memset(&data, 0, sizeof(data));
    return ok;
}

bool ctap2_pin_check_hash(const uint8_t* pin_hash) {
    U2fPinData data;
    if(!ctap2_pin_load(&data)) return false;
    if(data.retries == 0) {
        memset(&data, 0, sizeof(data));
        return false;
    }

    /* Spend the attempt BEFORE comparing, and persist it. Deciding first and
     * decrementing after would hand out a free guess to anyone willing to cut
     * the power in the microseconds between the two. */
    data.retries--;
    ctap2_pin_save(&data);

    uint8_t diff = 0;
    for(size_t i = 0; i < CTAP2_PIN_HASH_SIZE; i++) {
        diff |= (uint8_t)(data.pin_hash[i] ^ pin_hash[i]);
    }

    bool ok = (diff == 0);
    if(ok) {
        data.retries = CTAP2_PIN_MAX_RETRIES;
        ctap2_pin_save(&data);
    }

    memset(&data, 0, sizeof(data));
    return ok;
}

bool ctap2_pin_check(const uint8_t* pin, size_t pin_len) {
    if(pin_len < CTAP2_PIN_MIN_LEN || pin_len > CTAP2_PIN_MAX_LEN) return false;
    uint8_t hash[CTAP2_PIN_HASH_SIZE];
    ctap2_pin_hash(pin, pin_len, hash);
    bool ok = ctap2_pin_check_hash(hash);
    memset(hash, 0, sizeof(hash));
    return ok;
}

bool ctap2_pin_erase(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FS_Error err = storage_common_remove(storage, U2F_PIN_FILE);
    furi_record_close(RECORD_STORAGE);
    /* Already absent is success -- authenticatorReset must not fail because
     * there was no PIN to remove. */
    return (err == FSE_OK) || (err == FSE_NOT_EXIST);
}

/* --------------------------------------------------------- instance */

Ctap2Pin* ctap2_pin_alloc(void) {
    Ctap2Pin* instance = calloc(1, sizeof(Ctap2Pin));
    mbedtls_mpi_init(&instance->agreement_private);
    mbedtls_ecp_point_init(&instance->agreement_public);
    return instance;
}

void ctap2_pin_free(Ctap2Pin* instance) {
    furi_assert(instance);
    mbedtls_mpi_free(&instance->agreement_private);
    mbedtls_ecp_point_free(&instance->agreement_public);
    memset(instance, 0, sizeof(Ctap2Pin));
    free(instance);
}

void ctap2_pin_revoke_token(Ctap2Pin* instance) {
    memset(instance->pin_token, 0, sizeof(instance->pin_token));
    instance->pin_token_valid = false;
}

/* Generated once per boot and reused: the platform fetches it with
 * getKeyAgreement immediately before each operation. */
static bool ctap2_pin_ensure_agreement_key(Ctap2Pin* instance, mbedtls_ecp_group* group) {
    if(instance->agreement_ready) return true;

    int rc = mbedtls_ecp_gen_keypair(
        group, &instance->agreement_private, &instance->agreement_public, u2f_uecc_random_cb, NULL);
    if(rc != 0) {
        FURI_LOG_E(TAG, "ECDH keypair generation failed: %d", rc);
        return false;
    }
    instance->agreement_ready = true;
    return true;
}

/* sharedSecret = SHA-256(Z.x), where Z is the ECDH point. Protocol v1 takes
 * only the X coordinate and hashes it; there is no HKDF and no info string. */
static bool ctap2_pin_shared_secret(
    Ctap2Pin* instance,
    mbedtls_ecp_group* group,
    const uint8_t* peer_x,
    const uint8_t* peer_y,
    uint8_t* out) {
    mbedtls_ecp_point peer;
    mbedtls_mpi z;
    bool ok = false;

    mbedtls_ecp_point_init(&peer);
    mbedtls_mpi_init(&z);

    do {
        /* Rebuild the peer point from an uncompressed SEC1 encoding. */
        uint8_t encoded[1 + 2 * P256_COORD_SIZE];
        encoded[0] = 0x04;
        memcpy(encoded + 1, peer_x, P256_COORD_SIZE);
        memcpy(encoded + 1 + P256_COORD_SIZE, peer_y, P256_COORD_SIZE);

        if(mbedtls_ecp_point_read_binary(group, &peer, encoded, sizeof(encoded)) != 0) break;
        /* Reject a point that is not on the curve. Skipping this is the
         * classic invalid-curve attack: a crafted "public key" would leak the
         * device's ephemeral private scalar through the shared secret. */
        if(mbedtls_ecp_check_pubkey(group, &peer) != 0) {
            FURI_LOG_E(TAG, "Peer key is not a valid P-256 point");
            break;
        }

        if(mbedtls_ecdh_compute_shared(
               group, &z, &peer, &instance->agreement_private, u2f_uecc_random_cb, NULL) != 0)
            break;

        uint8_t zx[P256_COORD_SIZE];
        if(mbedtls_mpi_write_binary(&z, zx, sizeof(zx)) != 0) break;

        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        mbedtls_sha256_update(&sha, zx, sizeof(zx));
        mbedtls_sha256_finish(&sha, out);
        mbedtls_sha256_free(&sha);
        memset(zx, 0, sizeof(zx));
        ok = true;
    } while(0);

    mbedtls_ecp_point_free(&peer);
    mbedtls_mpi_free(&z);
    return ok;
}

/* Protocol v1 uses AES-256-CBC with an all-zero IV throughout. */
static bool ctap2_pin_aes(
    const uint8_t* key,
    int mode,
    const uint8_t* in,
    uint8_t* out,
    size_t len) {
    if(len == 0 || (len % AES_BLOCK_SIZE) != 0) return false;

    uint8_t iv[AES_BLOCK_SIZE];
    memset(iv, 0, sizeof(iv));

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);

    int rc = (mode == MBEDTLS_AES_ENCRYPT) ?
                 mbedtls_aes_setkey_enc(&ctx, key, SHARED_SECRET_SIZE * 8) :
                 mbedtls_aes_setkey_dec(&ctx, key, SHARED_SECRET_SIZE * 8);
    if(rc == 0) rc = mbedtls_aes_crypt_cbc(&ctx, mode, len, iv, in, out);

    mbedtls_aes_free(&ctx);
    return rc == 0;
}

static void ctap2_pin_hmac(
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
    if(b != NULL && b_len > 0) MCHECK(mbedtls_md_hmac_update(&ctx, b, b_len));
    MCHECK(mbedtls_md_hmac_finish(&ctx, out));
    mbedtls_md_free(&ctx);
}

static bool ctap2_pin_equal_ct(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for(size_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* pinUvAuthParam is the left 16 bytes of an HMAC. */
static bool ctap2_pin_verify_param(
    const uint8_t* key,
    const uint8_t* a,
    size_t a_len,
    const uint8_t* b,
    size_t b_len,
    const uint8_t* param,
    size_t param_len) {
    if(param_len != 16) return false;
    uint8_t expected[32];
    ctap2_pin_hmac(key, SHARED_SECRET_SIZE, a, a_len, b, b_len, expected);
    bool ok = ctap2_pin_equal_ct(expected, param, 16);
    memset(expected, 0, sizeof(expected));
    return ok;
}

bool ctap2_pin_verify_auth_param(
    Ctap2Pin* instance,
    const uint8_t* data,
    size_t data_len,
    const uint8_t* param,
    size_t param_len) {
    if(!instance->pin_token_valid || param_len != 16) return false;

    uint8_t expected[32];
    ctap2_pin_hmac(
        instance->pin_token, CTAP2_PIN_TOKEN_SIZE, data, data_len, NULL, 0, expected);
    bool ok = ctap2_pin_equal_ct(expected, param, 16);
    memset(expected, 0, sizeof(expected));
    return ok;
}

/* ------------------------------------------------- request parsing */

typedef struct {
    uint64_t protocol;
    uint64_t subcommand;
    bool has_peer_key;
    uint8_t peer_x[P256_COORD_SIZE];
    uint8_t peer_y[P256_COORD_SIZE];
    const uint8_t* pin_auth;
    size_t pin_auth_len;
    const uint8_t* new_pin_enc;
    size_t new_pin_enc_len;
    const uint8_t* pin_hash_enc;
    size_t pin_hash_enc_len;
} Ctap2PinReq;

static bool ctap2_pin_read_cose_key(CborReader* r, Ctap2PinReq* out) {
    size_t fields = 0;
    if(!cbor_r_map(r, &fields)) return false;

    bool have_x = false;
    bool have_y = false;

    for(size_t i = 0; i < fields; i++) {
        int64_t label = 0;
        if(!cbor_r_int(r, &label)) return false;

        if(label == COSE_KEY_X || label == COSE_KEY_Y) {
            const uint8_t* coord = NULL;
            size_t coord_len = 0;
            if(!cbor_r_bstr(r, &coord, &coord_len)) return false;
            if(coord_len != P256_COORD_SIZE) return false;
            if(label == COSE_KEY_X) {
                memcpy(out->peer_x, coord, P256_COORD_SIZE);
                have_x = true;
            } else {
                memcpy(out->peer_y, coord, P256_COORD_SIZE);
                have_y = true;
            }
        } else if(!cbor_r_skip(r)) {
            /* kty, alg, crv -- the curve is implied by protocol v1 and the
             * point is validated against the group on use, so there is nothing
             * these could tell us that check_pubkey does not. */
            return false;
        }
    }

    out->has_peer_key = have_x && have_y;
    return out->has_peer_key;
}

static uint8_t ctap2_pin_parse(const uint8_t* req, size_t req_len, Ctap2PinReq* out) {
    CborReader r;
    cbor_r_init(&r, req, req_len);

    size_t entries = 0;
    if(!cbor_r_map(&r, &entries)) return CTAP2_ERR_INVALID_CBOR;

    memset(out, 0, sizeof(*out));

    for(size_t i = 0; i < entries; i++) {
        uint64_t key = 0;
        if(!cbor_r_uint(&r, &key)) return CTAP2_ERR_INVALID_CBOR;

        bool ok = true;
        switch(key) {
        case PIN_KEY_PROTOCOL:
            ok = cbor_r_uint(&r, &out->protocol);
            break;
        case PIN_KEY_SUBCOMMAND:
            ok = cbor_r_uint(&r, &out->subcommand);
            break;
        case PIN_KEY_KEY_AGREEMENT:
            ok = ctap2_pin_read_cose_key(&r, out);
            break;
        case PIN_KEY_PIN_AUTH:
            ok = cbor_r_bstr(&r, &out->pin_auth, &out->pin_auth_len);
            break;
        case PIN_KEY_NEW_PIN_ENC:
            ok = cbor_r_bstr(&r, &out->new_pin_enc, &out->new_pin_enc_len);
            break;
        case PIN_KEY_PIN_HASH_ENC:
            ok = cbor_r_bstr(&r, &out->pin_hash_enc, &out->pin_hash_enc_len);
            break;
        default:
            ok = cbor_r_skip(&r);
            break;
        }

        if(!ok) return CTAP2_ERR_INVALID_CBOR;
    }

    if(out->subcommand == 0) return CTAP2_ERR_MISSING_PARAMETER;
    return CTAP2_OK;
}

/* Decrypt newPinEnc and strip the zero padding. */
static uint8_t ctap2_pin_decode_new_pin(
    const uint8_t* shared_secret,
    const uint8_t* enc,
    size_t enc_len,
    uint8_t* pin_out,
    size_t* pin_len_out) {
    if(enc_len < PIN_PADDED_LEN || enc_len > CTAP2_PIN_MAX_LEN + 1 + AES_BLOCK_SIZE * 4)
        return CTAP2_ERR_PIN_POLICY_VIOLATION;
    if((enc_len % AES_BLOCK_SIZE) != 0) return CTAP1_ERR_INVALID_LENGTH;

    uint8_t padded[PIN_PADDED_LEN + AES_BLOCK_SIZE * 4];
    if(enc_len > sizeof(padded)) return CTAP2_ERR_PIN_POLICY_VIOLATION;

    if(!ctap2_pin_aes(shared_secret, MBEDTLS_AES_DECRYPT, enc, padded, enc_len))
        return CTAP1_ERR_OTHER;

    /* The PIN is zero-padded, so its length is the offset of the first NUL. */
    size_t len = 0;
    while(len < enc_len && padded[len] != 0) len++;

    if(len < CTAP2_PIN_MIN_LEN || len > CTAP2_PIN_MAX_LEN) {
        memset(padded, 0, sizeof(padded));
        return CTAP2_ERR_PIN_POLICY_VIOLATION;
    }

    memcpy(pin_out, padded, len);
    *pin_len_out = len;
    memset(padded, 0, sizeof(padded));
    return CTAP2_OK;
}

/* Shared preamble for the three subcommands that carry a platform key. */
static uint8_t ctap2_pin_derive_shared(
    Ctap2Pin* instance,
    mbedtls_ecp_group* group,
    const Ctap2PinReq* parsed,
    uint8_t* shared_secret) {
    if(!parsed->has_peer_key) return CTAP2_ERR_MISSING_PARAMETER;
    if(!ctap2_pin_ensure_agreement_key(instance, group)) return CTAP1_ERR_OTHER;
    if(!ctap2_pin_shared_secret(instance, group, parsed->peer_x, parsed->peer_y, shared_secret))
        return CTAP1_ERR_INVALID_PARAMETER;
    return CTAP2_OK;
}

/* --------------------------------------------------- subcommands */

static size_t ctap2_pin_write_retries(uint8_t* resp, size_t resp_cap) {
    CborWriter w;
    cbor_w_init(&w, resp + 1, resp_cap - 1);
    cbor_w_map(&w, 1);
    cbor_w_uint(&w, 3); /* pinRetries */
    cbor_w_uint(&w, ctap2_pin_get_retries());

    size_t len = cbor_w_finish(&w);
    resp[0] = (len == 0) ? CTAP1_ERR_OTHER : CTAP2_OK;
    return (len == 0) ? 1 : len + 1;
}

static size_t ctap2_pin_write_key_agreement(
    Ctap2Pin* instance,
    mbedtls_ecp_group* group,
    uint8_t* resp,
    size_t resp_cap) {
    if(!ctap2_pin_ensure_agreement_key(instance, group)) {
        resp[0] = CTAP1_ERR_OTHER;
        return 1;
    }

    uint8_t encoded[1 + 2 * P256_COORD_SIZE];
    size_t olen = 0;
    if(mbedtls_ecp_point_write_binary(
           group,
           &instance->agreement_public,
           MBEDTLS_ECP_PF_UNCOMPRESSED,
           &olen,
           encoded,
           sizeof(encoded)) != 0 ||
       olen != sizeof(encoded)) {
        resp[0] = CTAP1_ERR_OTHER;
        return 1;
    }

    CborWriter w;
    cbor_w_init(&w, resp + 1, resp_cap - 1);
    cbor_w_map(&w, 1);
    cbor_w_uint(&w, 1); /* keyAgreement */

    /* Same canonical one-byte label order as any COSE key, but the algorithm
     * is ECDH-ES+HKDF-256 rather than ES256 -- this key agrees, it does not
     * sign. */
    cbor_w_map(&w, 5);
    cbor_w_int(&w, COSE_KEY_KTY);
    cbor_w_int(&w, COSE_KTY_EC2);
    cbor_w_int(&w, COSE_KEY_ALG);
    cbor_w_int(&w, COSE_ALG_ECDH_ES_HKDF256);
    cbor_w_int(&w, COSE_KEY_CRV);
    cbor_w_int(&w, COSE_CRV_P256);
    cbor_w_int(&w, COSE_KEY_X);
    cbor_w_bstr(&w, encoded + 1, P256_COORD_SIZE);
    cbor_w_int(&w, COSE_KEY_Y);
    cbor_w_bstr(&w, encoded + 1 + P256_COORD_SIZE, P256_COORD_SIZE);

    size_t len = cbor_w_finish(&w);
    resp[0] = (len == 0) ? CTAP1_ERR_OTHER : CTAP2_OK;
    return (len == 0) ? 1 : len + 1;
}

static uint8_t ctap2_pin_do_set(
    Ctap2Pin* instance,
    mbedtls_ecp_group* group,
    const Ctap2PinReq* parsed) {
    if(ctap2_pin_is_set()) return CTAP2_ERR_PIN_AUTH_INVALID;
    if(parsed->new_pin_enc == NULL || parsed->pin_auth == NULL)
        return CTAP2_ERR_MISSING_PARAMETER;

    uint8_t shared_secret[SHARED_SECRET_SIZE];
    uint8_t status = ctap2_pin_derive_shared(instance, group, parsed, shared_secret);
    if(status != CTAP2_OK) return status;

    uint8_t result = CTAP2_ERR_PIN_AUTH_INVALID;
    do {
        if(!ctap2_pin_verify_param(
               shared_secret,
               parsed->new_pin_enc,
               parsed->new_pin_enc_len,
               NULL,
               0,
               parsed->pin_auth,
               parsed->pin_auth_len))
            break;

        uint8_t pin[CTAP2_PIN_MAX_LEN];
        size_t pin_len = 0;
        result = ctap2_pin_decode_new_pin(
            shared_secret, parsed->new_pin_enc, parsed->new_pin_enc_len, pin, &pin_len);
        if(result == CTAP2_OK) {
            result = ctap2_pin_store(pin, pin_len) ? CTAP2_OK : CTAP1_ERR_OTHER;
            if(result == CTAP2_OK) ctap2_pin_revoke_token(instance);
        }
        memset(pin, 0, sizeof(pin));
    } while(0);

    memset(shared_secret, 0, sizeof(shared_secret));
    return result;
}

/* Shared by changePIN and getPINToken: decrypt the supplied pinHash and check
 * it, honouring both the per-boot and the absolute retry limits. */
static uint8_t ctap2_pin_verify_supplied_hash(
    Ctap2Pin* instance,
    const uint8_t* shared_secret,
    const uint8_t* pin_hash_enc,
    size_t pin_hash_enc_len) {
    if(pin_hash_enc == NULL || pin_hash_enc_len != AES_BLOCK_SIZE)
        return CTAP2_ERR_MISSING_PARAMETER;
    if(!ctap2_pin_is_set()) return CTAP2_ERR_PIN_NOT_SET;
    if(ctap2_pin_get_retries() == 0) return CTAP2_ERR_PIN_BLOCKED;
    if(instance->boot_failures >= CTAP2_PIN_RETRIES_PER_BOOT) return CTAP2_ERR_PIN_AUTH_BLOCKED;

    uint8_t pin_hash[CTAP2_PIN_HASH_SIZE];
    if(!ctap2_pin_aes(
           shared_secret, MBEDTLS_AES_DECRYPT, pin_hash_enc, pin_hash, CTAP2_PIN_HASH_SIZE))
        return CTAP1_ERR_OTHER;

    bool ok = ctap2_pin_check_hash(pin_hash);
    memset(pin_hash, 0, sizeof(pin_hash));

    if(ok) {
        instance->boot_failures = 0;
        return CTAP2_OK;
    }

    instance->boot_failures++;

    /* Report the strongest applicable block so the client stops retrying and
     * tells the user what actually has to happen -- a reboot, or a reset. */
    if(ctap2_pin_get_retries() == 0) return CTAP2_ERR_PIN_BLOCKED;
    if(instance->boot_failures >= CTAP2_PIN_RETRIES_PER_BOOT) return CTAP2_ERR_PIN_AUTH_BLOCKED;
    return CTAP2_ERR_PIN_INVALID;
}

static uint8_t ctap2_pin_do_change(
    Ctap2Pin* instance,
    mbedtls_ecp_group* group,
    const Ctap2PinReq* parsed) {
    if(parsed->new_pin_enc == NULL || parsed->pin_hash_enc == NULL || parsed->pin_auth == NULL)
        return CTAP2_ERR_MISSING_PARAMETER;

    uint8_t shared_secret[SHARED_SECRET_SIZE];
    uint8_t status = ctap2_pin_derive_shared(instance, group, parsed, shared_secret);
    if(status != CTAP2_OK) return status;

    uint8_t result;
    do {
        /* The MAC covers both ciphertexts, so a mix-and-match of a fresh
         * newPinEnc with a captured pinHashEnc does not verify. */
        if(!ctap2_pin_verify_param(
               shared_secret,
               parsed->new_pin_enc,
               parsed->new_pin_enc_len,
               parsed->pin_hash_enc,
               parsed->pin_hash_enc_len,
               parsed->pin_auth,
               parsed->pin_auth_len)) {
            result = CTAP2_ERR_PIN_AUTH_INVALID;
            break;
        }

        result = ctap2_pin_verify_supplied_hash(
            instance, shared_secret, parsed->pin_hash_enc, parsed->pin_hash_enc_len);
        if(result != CTAP2_OK) break;

        uint8_t pin[CTAP2_PIN_MAX_LEN];
        size_t pin_len = 0;
        result = ctap2_pin_decode_new_pin(
            shared_secret, parsed->new_pin_enc, parsed->new_pin_enc_len, pin, &pin_len);
        if(result == CTAP2_OK) {
            result = ctap2_pin_store(pin, pin_len) ? CTAP2_OK : CTAP1_ERR_OTHER;
            if(result == CTAP2_OK) ctap2_pin_revoke_token(instance);
        }
        memset(pin, 0, sizeof(pin));
    } while(0);

    memset(shared_secret, 0, sizeof(shared_secret));
    return result;
}

static size_t ctap2_pin_do_get_token(
    Ctap2Pin* instance,
    mbedtls_ecp_group* group,
    const Ctap2PinReq* parsed,
    uint8_t* resp,
    size_t resp_cap) {
    uint8_t shared_secret[SHARED_SECRET_SIZE];
    uint8_t status = ctap2_pin_derive_shared(instance, group, parsed, shared_secret);
    if(status != CTAP2_OK) {
        memset(shared_secret, 0, sizeof(shared_secret));
        resp[0] = status;
        return 1;
    }

    status = ctap2_pin_verify_supplied_hash(
        instance, shared_secret, parsed->pin_hash_enc, parsed->pin_hash_enc_len);
    if(status != CTAP2_OK) {
        memset(shared_secret, 0, sizeof(shared_secret));
        resp[0] = status;
        return 1;
    }

    /* A fresh token per successful PIN entry, so one captured earlier cannot
     * be reused after the PIN changes. */
    furi_hal_random_fill_buf(instance->pin_token, CTAP2_PIN_TOKEN_SIZE);
    instance->pin_token_valid = true;

    uint8_t token_enc[CTAP2_PIN_TOKEN_SIZE];
    bool ok = ctap2_pin_aes(
        shared_secret, MBEDTLS_AES_ENCRYPT, instance->pin_token, token_enc, CTAP2_PIN_TOKEN_SIZE);
    memset(shared_secret, 0, sizeof(shared_secret));

    if(!ok) {
        resp[0] = CTAP1_ERR_OTHER;
        return 1;
    }

    CborWriter w;
    cbor_w_init(&w, resp + 1, resp_cap - 1);
    cbor_w_map(&w, 1);
    cbor_w_uint(&w, 2); /* pinToken */
    cbor_w_bstr(&w, token_enc, sizeof(token_enc));
    memset(token_enc, 0, sizeof(token_enc));

    size_t len = cbor_w_finish(&w);
    resp[0] = (len == 0) ? CTAP1_ERR_OTHER : CTAP2_OK;
    return (len == 0) ? 1 : len + 1;
}

/* --------------------------------------------------------- dispatch */

size_t ctap2_pin_command(
    Ctap2Pin* instance,
    mbedtls_ecp_group* group,
    const uint8_t* req,
    size_t req_len,
    uint8_t* resp,
    size_t resp_cap) {
    furi_assert(instance);

    Ctap2PinReq parsed;
    uint8_t status = ctap2_pin_parse(req, req_len, &parsed);
    if(status != CTAP2_OK) {
        resp[0] = status;
        return 1;
    }

    /* getPinRetries carries no protocol field in some client implementations,
     * so the version is only enforced where it actually matters. */
    if(parsed.subcommand != PIN_SUB_GET_RETRIES && parsed.protocol != 1) {
        resp[0] = CTAP1_ERR_INVALID_PARAMETER;
        return 1;
    }

    switch(parsed.subcommand) {
    case PIN_SUB_GET_RETRIES:
        return ctap2_pin_write_retries(resp, resp_cap);

    case PIN_SUB_GET_KEY_AGREE:
        return ctap2_pin_write_key_agreement(instance, group, resp, resp_cap);

    case PIN_SUB_SET_PIN:
        resp[0] = ctap2_pin_do_set(instance, group, &parsed);
        return 1;

    case PIN_SUB_CHANGE_PIN:
        resp[0] = ctap2_pin_do_change(instance, group, &parsed);
        return 1;

    case PIN_SUB_GET_PIN_TOKEN:
        return ctap2_pin_do_get_token(instance, group, &parsed, resp, resp_cap);

    default:
        resp[0] = CTAP1_ERR_INVALID_PARAMETER;
        return 1;
    }
}
