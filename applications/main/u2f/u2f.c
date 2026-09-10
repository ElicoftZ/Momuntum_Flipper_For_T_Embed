#include "u2f.h"
#include "u2f_data.h"
#include "u2f_ecc.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_random.h>

#include <mbedtls/sha256.h>
#include <mbedtls/md.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/error.h>

#define TAG "U2f"

#define WORKER_TAG TAG "Worker"

#define MCHECK(expr) furi_check((expr) == 0)

#define U2F_CMD_REGISTER     0x01
#define U2F_CMD_AUTHENTICATE 0x02
#define U2F_CMD_VERSION      0x03

typedef enum {
    U2fCheckOnly = 0x07, // "check-only" - only check key handle, don't send auth response
    U2fEnforce =
        0x03, // "enforce-user-presence-and-sign" - send auth response only if user is present
    U2fDontEnforce =
        0x08, // "dont-enforce-user-presence-and-sign" - send auth response even if user is missing
} U2fAuthMode;

#define U2F_HASH_SIZE      32
#define U2F_NONCE_SIZE     32
#define U2F_CHALLENGE_SIZE 32
#define U2F_APP_ID_SIZE    32

typedef struct {
    uint8_t len;
    uint8_t hash[U2F_HASH_SIZE];
    uint8_t nonce[U2F_NONCE_SIZE];
} FURI_PACKED U2fKeyHandle;

typedef struct {
    uint8_t cla;
    uint8_t ins;
    uint8_t p1;
    uint8_t p2;
    uint8_t len[3];
    uint8_t challenge[U2F_CHALLENGE_SIZE];
    uint8_t app_id[U2F_APP_ID_SIZE];
} FURI_PACKED U2fRegisterReq;

typedef struct {
    uint8_t reserved;
    U2fPubKey pub_key;
    U2fKeyHandle key_handle;
    uint8_t cert[];
} FURI_PACKED U2fRegisterResp;

typedef struct {
    uint8_t cla;
    uint8_t ins;
    uint8_t p1;
    uint8_t p2;
    uint8_t len[3];
    uint8_t challenge[U2F_CHALLENGE_SIZE];
    uint8_t app_id[U2F_APP_ID_SIZE];
    U2fKeyHandle key_handle;
} FURI_PACKED U2fAuthReq;

typedef struct {
    uint8_t user_present;
    uint32_t counter;
    uint8_t signature[];
} FURI_PACKED U2fAuthResp;

/* The register response is assembled inside the U2FHID payload buffer, after
 * a fixed 67-byte header and before ~72 bytes of DER signature plus status. */
#define U2F_CERT_MAX_LEN 1024

static const uint8_t ver_str[] = {"U2F_V2"};

static const uint8_t state_no_error[] = {0x90, 0x00};
static const uint8_t state_not_supported[] = {0x6D, 0x00};
static const uint8_t state_user_missing[] = {0x69, 0x85};
static const uint8_t state_wrong_data[] = {0x6A, 0x80};

struct U2fData {
    uint8_t device_key[U2F_EC_KEY_SIZE];
    uint8_t cert_key[U2F_EC_KEY_SIZE];
    uint32_t counter;
    bool ready;
    /* Whether cert_key is actually the private half of the certificate we
     * ship. See u2f_cert_key_matches_cert(). */
    bool cert_key_valid;
    bool user_present;
    U2fEvtCallback callback;
    void* context;
    U2fPresenceCallback presence_callback;
    void* presence_context;
    /* USB and NFC each run their own worker over one U2fData. The signature
     * counter is the piece of shared mutable state that is persisted, and two
     * concurrent read-increment-write cycles would corrupt the file. */
    FuriMutex* counter_mutex;
    mbedtls_ecp_group group;
};

int u2f_uecc_random_cb(void* context, uint8_t* dest, unsigned size) {
    UNUSED(context);
    furi_hal_random_fill_buf(dest, size);
    return 0;
}

U2fData* u2f_alloc(void) {
    /* calloc, not malloc: the presence callback is consulted before anything
     * assigns it, and furi's malloc does not zero. */
    return calloc(1, sizeof(U2fData));
}

void u2f_free(U2fData* U2F) {
    furi_assert(U2F);
    if(U2F->counter_mutex != NULL) furi_mutex_free(U2F->counter_mutex);
    mbedtls_ecp_group_free(&U2F->group);
    free(U2F);
}

/* Does the attestation key actually belong to the attestation certificate?
 *
 * It is not a rhetorical question. The key file is sealed with the enclave's
 * device-unique key and AES-CBC carries no integrity check, so unsealing it
 * with the WRONG key succeeds and returns 32 bytes of noise. Everything
 * downstream then works perfectly and signs with a key the certificate has
 * never heard of, and the only symptom is a relying party reporting that the
 * authenticator "did not prove possession of its private key" -- nothing on
 * the device, nothing in the log.
 *
 * Any change to how the enclave key is derived puts every previously sealed
 * file into exactly that state, which is how this was found. Checking the pair
 * costs one point multiplication once per session. */
static bool u2f_cert_key_matches_cert(U2fData* U2F) {
    uint8_t* cert = malloc(U2F_CERT_MAX_LEN);
    uint32_t cert_len = u2f_data_cert_load(cert, U2F_CERT_MAX_LEN);
    bool match = false;

    if(cert_len >= sizeof(U2fPubKey) + 3) {
        U2fPubKey pub_key;
        u2f_ecc_compute_public_key(&U2F->group, U2F->cert_key, &pub_key);

        /* The public key of a P-256 certificate sits in its SPKI as a BIT
         * STRING of 66 bytes with no unused bits, followed by the uncompressed
         * point: 03 42 00 04. Distinctive enough to find without dragging a
         * whole X.509 parser in for one field. */
        for(uint32_t i = 0; i + sizeof(U2fPubKey) + 3 <= cert_len; i++) {
            if(cert[i] == 0x03 && cert[i + 1] == 0x42 && cert[i + 2] == 0x00 &&
               cert[i + 3] == 0x04) {
                match = memcmp(&cert[i + 3], &pub_key, sizeof(U2fPubKey)) == 0;
                break;
            }
        }
        memset(&pub_key, 0, sizeof(pub_key));
    }

    free(cert);
    return match;
}

bool u2f_init(U2fData* U2F) {
    furi_assert(U2F);

    bool migrated_legacy_key = false;
    bool migrated_plaintext_key = false;
    bool migration_write_ok = true;

    if(u2f_data_cert_check() == false) {
        FURI_LOG_E(TAG, "Certificate load error");
        return false;
    }
    if(u2f_data_cert_key_load(U2F->cert_key) == false) {
        FURI_LOG_E(TAG, "Certificate key load error");
        return false;
    }

    /* Validate the attestation key before opening the other encrypted files.
     * If the certificate proves that the earlier MAC-derived key is correct,
     * migrate the whole U2F data set together so registrations and counters
     * remain valid. */
    mbedtls_ecp_group_init(&U2F->group);
    mbedtls_ecp_group_load(&U2F->group, MBEDTLS_ECP_DP_SECP256R1);
    U2F->cert_key_valid = u2f_cert_key_matches_cert(U2F);
    if(U2F->cert_key_valid == false && u2f_data_cert_key_load_legacy(U2F->cert_key)) {
        U2F->cert_key_valid = u2f_cert_key_matches_cert(U2F);
        migrated_legacy_key = U2F->cert_key_valid;
    }
    if(U2F->cert_key_valid == false &&
       u2f_data_cert_key_load_plaintext_legacy(U2F->cert_key)) {
        U2F->cert_key_valid = u2f_cert_key_matches_cert(U2F);
        migrated_plaintext_key = U2F->cert_key_valid;
    }
    if(migrated_legacy_key || migrated_plaintext_key) {
        FURI_LOG_W(
            TAG,
            "Migrating U2F files from the legacy %s key",
            migrated_plaintext_key ? "passthrough" : "device");
    }

    const bool device_key_loaded =
        migrated_plaintext_key ? u2f_data_key_load_plaintext_legacy(U2F->device_key) :
        migrated_legacy_key    ? u2f_data_key_load_legacy(U2F->device_key) :
                                 u2f_data_key_load(U2F->device_key);
    if(device_key_loaded == false) {
        FURI_LOG_W(TAG, "Key loading error, generating new");
        if(u2f_data_key_generate(U2F->device_key) == false) {
            FURI_LOG_E(TAG, "Key write failed");
            return false;
        }
    } else if((migrated_legacy_key || migrated_plaintext_key) &&
              !u2f_data_key_reencrypt(U2F->device_key)) {
        FURI_LOG_W(TAG, "Device key works, but migration write failed");
        migration_write_ok = false;
    }

    const bool counter_loaded =
        migrated_plaintext_key ? u2f_data_cnt_read_plaintext_legacy(&U2F->counter) :
        migrated_legacy_key    ? u2f_data_cnt_read_legacy(&U2F->counter) :
                                 u2f_data_cnt_read(&U2F->counter);
    if(counter_loaded == false) {
        FURI_LOG_W(TAG, "Counter loading error, resetting counter");
        U2F->counter = 0;
        if(u2f_data_cnt_write(0) == false) {
            FURI_LOG_E(TAG, "Counter write failed");
            return false;
        }
    } else if((migrated_legacy_key || migrated_plaintext_key) &&
              !u2f_data_cnt_write(U2F->counter)) {
        FURI_LOG_W(TAG, "Counter works, but migration write failed");
        migration_write_ok = false;
    }

    /* The certificate key is the migration marker because it is the only key
     * we can validate against public data. Write it last: if power is lost
     * while converting the device key or counter, the next launch will still
     * detect the legacy certificate and retry the complete migration. */
    if((migrated_legacy_key || migrated_plaintext_key) && migration_write_ok &&
       !u2f_data_cert_key_reencrypt(U2F->cert_key)) {
        FURI_LOG_W(TAG, "Certificate key works, but migration write failed");
    }

    /* Not fatal: CTAP2 self-attests and needs no certificate at all. Only the
     * CTAP1 path, whose response format requires one, has to refuse. */
    if(U2F->cert_key_valid == false) {
        FURI_LOG_E(
            TAG,
            "Attestation key does not match the certificate. Re-copy cert_key.u2f "
            "(Type 2) to the card so it is re-sealed with the current device key.");
    }

    U2F->counter_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    U2F->ready = true;
    return true;
}

void u2f_set_event_callback(U2fData* U2F, U2fEvtCallback callback, void* context) {
    furi_assert(U2F);
    furi_assert(callback);
    U2F->callback = callback;
    U2F->context = context;
}

void u2f_confirm_user_present(U2fData* U2F) {
    U2F->user_present = true;
    /* Wake the transport worker. CTAP2 blocks there sending KEEPALIVEs and has
     * no host-side retry to fall back on, so the flag alone is not enough. */
    if(U2F->presence_callback != NULL) U2F->presence_callback(U2F->presence_context);
}

void u2f_set_presence_callback(U2fData* U2F, U2fPresenceCallback callback, void* context) {
    furi_assert(U2F);
    U2F->presence_callback = callback;
    U2F->presence_context = context;
}

bool u2f_is_user_present(U2fData* U2F) {
    return U2F->user_present;
}

void u2f_clear_user_present(U2fData* U2F) {
    U2F->user_present = false;
}

const uint8_t* u2f_get_device_key(U2fData* U2F) {
    furi_assert(U2F);
    return U2F->device_key;
}

const uint8_t* u2f_get_cert_key(U2fData* U2F) {
    furi_assert(U2F);
    return U2F->cert_key;
}

struct mbedtls_ecp_group* u2f_get_group(U2fData* U2F) {
    furi_assert(U2F);
    return &U2F->group;
}

uint32_t u2f_get_counter(U2fData* U2F) {
    furi_assert(U2F);
    return U2F->counter;
}

uint32_t u2f_bump_counter(U2fData* U2F) {
    furi_assert(U2F);

    if(U2F->counter_mutex != NULL) {
        furi_mutex_acquire(U2F->counter_mutex, FuriWaitForever);
    }
    U2F->counter++;
    uint32_t value = U2F->counter;
    u2f_data_cnt_write(value);
    if(U2F->counter_mutex != NULL) {
        furi_mutex_release(U2F->counter_mutex);
    }

    return value;
}

bool u2f_regenerate_device_key(U2fData* U2F) {
    furi_assert(U2F);

    uint8_t new_key[U2F_EC_KEY_SIZE];
    if(!u2f_data_key_generate(new_key)) {
        FURI_LOG_E(TAG, "Device key regeneration failed");
        return false;
    }
    memcpy(U2F->device_key, new_key, sizeof(U2F->device_key));
    memset(new_key, 0, sizeof(new_key));

    /* The counter must go back to zero with the key. Leaving it high would
     * make the first assertion under the new key look like a clone to any
     * relying party still holding the old counter value. */
    U2F->counter = 0;
    u2f_data_cnt_write(0);
    return true;
}

void u2f_notify(U2fData* U2F, U2fNotifyEvent evt) {
    furi_assert(U2F);
    if(U2F->callback != NULL) U2F->callback(evt, U2F->context);
}

static uint8_t u2f_der_encode_int(uint8_t* der, uint8_t* val, uint8_t val_len) {
    der[0] = 0x02; // Integer

    uint8_t len = 2;
    // Omit leading zeros
    while(val[0] == 0 && val_len > 0) {
        ++val;
        --val_len;
    }

    // Check if integer is negative
    if(val[0] > 0x7f) der[len++] = 0;

    memcpy(der + len, val, val_len);
    len += val_len;

    der[1] = len - 2;
    return len;
}

uint8_t u2f_der_encode_signature(uint8_t* der, uint8_t* sig) {
    der[0] = 0x30;

    uint8_t len = 2;
    len += u2f_der_encode_int(der + len, sig, U2F_HASH_SIZE);
    len += u2f_der_encode_int(der + len, sig + U2F_HASH_SIZE, U2F_HASH_SIZE);

    der[1] = len - 2;
    return len;
}

void u2f_ecc_sign(
    mbedtls_ecp_group* grp,
    const uint8_t* key,
    uint8_t* hash,
    uint8_t* signature) {
    mbedtls_mpi r, s, d;

    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    mbedtls_mpi_init(&d);

    MCHECK(mbedtls_mpi_read_binary(&d, key, U2F_EC_KEY_SIZE));
    MCHECK(mbedtls_ecdsa_sign(grp, &r, &s, &d, hash, U2F_HASH_SIZE, u2f_uecc_random_cb, NULL));
    MCHECK(mbedtls_mpi_write_binary(&r, signature, U2F_EC_BIGNUM_SIZE));
    MCHECK(mbedtls_mpi_write_binary(&s, signature + U2F_EC_BIGNUM_SIZE, U2F_EC_BIGNUM_SIZE));

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&d);
}

void u2f_ecc_compute_public_key(
    mbedtls_ecp_group* grp,
    const uint8_t* private_key,
    U2fPubKey* public_key) {
    mbedtls_ecp_point Q;
    mbedtls_mpi d;
    size_t olen;

    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&d);

    MCHECK(mbedtls_mpi_read_binary(&d, private_key, U2F_EC_KEY_SIZE));
    MCHECK(mbedtls_ecp_mul(grp, &Q, &d, &grp->G, u2f_uecc_random_cb, NULL));
    MCHECK(mbedtls_ecp_check_privkey(grp, &d));

    MCHECK(mbedtls_ecp_point_write_binary(
        grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, (unsigned char*)public_key, sizeof(U2fPubKey)));

    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
}

///////////////////////////////////////////

static uint16_t u2f_register(U2fData* U2F, uint8_t* buf) {
    U2fRegisterReq* req = (U2fRegisterReq*)buf;
    U2fRegisterResp* resp = (U2fRegisterResp*)buf;
    U2fKeyHandle handle;
    uint8_t private[U2F_EC_KEY_SIZE];
    U2fPubKey pub_key;
    uint8_t hash[U2F_HASH_SIZE];
    uint8_t signature[U2F_EC_BIGNUM_SIZE * 2];

    if(u2f_data_check(false) == false) {
        U2F->ready = false;
        if(U2F->callback != NULL) U2F->callback(U2fNotifyError, U2F->context);
        memcpy(&buf[0], state_not_supported, 2);
        return 2;
    }

    /* A U2F registration response has nowhere to put anything but a real
     * certificate and a signature by its key, so a mismatched pair can only
     * produce an attestation the relying party will reject. Say so instead. */
    if(U2F->cert_key_valid == false) {
        if(U2F->callback != NULL) U2F->callback(U2fNotifyError, U2F->context);
        memcpy(&buf[0], state_not_supported, 2);
        return 2;
    }

    if(U2F->callback != NULL) U2F->callback(U2fNotifyRegister, U2F->context);
    if(U2F->user_present == false) {
        memcpy(&buf[0], state_user_missing, 2);
        return 2;
    }
    U2F->user_present = false;

    handle.len = U2F_HASH_SIZE * 2;

    // Generate random nonce
    furi_hal_random_fill_buf(handle.nonce, 32);

    {
        mbedtls_md_context_t hmac_ctx;
        mbedtls_md_init(&hmac_ctx);
        MCHECK(mbedtls_md_setup(&hmac_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1));
        MCHECK(mbedtls_md_hmac_starts(&hmac_ctx, U2F->device_key, sizeof(U2F->device_key)));

        // Generate private key
        MCHECK(mbedtls_md_hmac_update(&hmac_ctx, req->app_id, sizeof(req->app_id)));
        MCHECK(mbedtls_md_hmac_update(&hmac_ctx, handle.nonce, sizeof(handle.nonce)));
        MCHECK(mbedtls_md_hmac_finish(&hmac_ctx, private));

        MCHECK(mbedtls_md_hmac_reset(&hmac_ctx));

        // Generate private key handle
        MCHECK(mbedtls_md_hmac_update(&hmac_ctx, private, sizeof(private)));
        MCHECK(mbedtls_md_hmac_update(&hmac_ctx, req->app_id, sizeof(req->app_id)));
        MCHECK(mbedtls_md_hmac_finish(&hmac_ctx, handle.hash));

        mbedtls_md_free(&hmac_ctx);
    }

    // Generate public key
    u2f_ecc_compute_public_key(&U2F->group, private, &pub_key);

    // Generate signature
    {
        uint8_t reserved_byte = 0;

        mbedtls_sha256_context sha_ctx;

        mbedtls_sha256_init(&sha_ctx);
        mbedtls_sha256_starts(&sha_ctx, 0);

        mbedtls_sha256_update(&sha_ctx, &reserved_byte, 1);
        mbedtls_sha256_update(&sha_ctx, req->app_id, sizeof(req->app_id));
        mbedtls_sha256_update(&sha_ctx, req->challenge, sizeof(req->challenge));
        mbedtls_sha256_update(&sha_ctx, handle.hash, handle.len);
        mbedtls_sha256_update(&sha_ctx, (uint8_t*)&pub_key, sizeof(U2fPubKey));

        mbedtls_sha256_finish(&sha_ctx, hash);
        mbedtls_sha256_free(&sha_ctx);
    }

    // Sign hash
    u2f_ecc_sign(&U2F->group, U2F->cert_key, hash, signature);

    // Encode response message
    resp->reserved = 0x05;
    memcpy(&(resp->pub_key), &pub_key, sizeof(U2fPubKey));
    memcpy(&(resp->key_handle), &handle, sizeof(U2fKeyHandle));
    uint32_t cert_len = u2f_data_cert_load(resp->cert, U2F_CERT_MAX_LEN);
    if(cert_len == 0) {
        memcpy(&buf[0], state_not_supported, 2);
        return 2;
    }
    uint8_t signature_len = u2f_der_encode_signature(resp->cert + cert_len, signature);
    memcpy(resp->cert + cert_len + signature_len, state_no_error, 2);

    return sizeof(U2fRegisterResp) + cert_len + signature_len + 2;
}

static inline uint32_t u2f_to_big_endian(uint32_t a) {
    return __builtin_bswap32(a);
}

static uint16_t u2f_authenticate(U2fData* U2F, uint8_t* buf) {
    U2fAuthReq* req = (U2fAuthReq*)buf;
    U2fAuthResp* resp = (U2fAuthResp*)buf;
    uint8_t priv_key[U2F_EC_KEY_SIZE];
    uint8_t mac_control[32];
    uint8_t flags = 0;
    uint8_t hash[U2F_HASH_SIZE];
    uint8_t signature[U2F_HASH_SIZE * 2];
    uint32_t be_u2f_counter;

    if(u2f_data_check(false) == false) {
        U2F->ready = false;
        if(U2F->callback != NULL) U2F->callback(U2fNotifyError, U2F->context);
        memcpy(&buf[0], state_not_supported, 2);
        return 2;
    }

    if(U2F->callback != NULL) U2F->callback(U2fNotifyAuth, U2F->context);
    if(U2F->user_present == true) {
        flags |= 1;
    } else {
        if(req->p1 == U2fEnforce) {
            memcpy(&buf[0], state_user_missing, 2);
            return 2;
        }
    }
    U2F->user_present = false;

    // The 4 byte counter is represented in big endian. Increment it before use
    be_u2f_counter = u2f_to_big_endian(U2F->counter + 1);

    // Generate hash
    {
        mbedtls_sha256_context sha_ctx;

        mbedtls_sha256_init(&sha_ctx);
        mbedtls_sha256_starts(&sha_ctx, 0);

        mbedtls_sha256_update(&sha_ctx, req->app_id, sizeof(req->app_id));
        mbedtls_sha256_update(&sha_ctx, &flags, 1);
        mbedtls_sha256_update(&sha_ctx, (uint8_t*)&(be_u2f_counter), sizeof(be_u2f_counter));
        mbedtls_sha256_update(&sha_ctx, req->challenge, sizeof(req->challenge));

        mbedtls_sha256_finish(&sha_ctx, hash);
        mbedtls_sha256_free(&sha_ctx);
    }

    {
        mbedtls_md_context_t hmac_ctx;
        mbedtls_md_init(&hmac_ctx);
        MCHECK(mbedtls_md_setup(&hmac_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1));
        MCHECK(mbedtls_md_hmac_starts(&hmac_ctx, U2F->device_key, sizeof(U2F->device_key)));

        // Recover private key
        MCHECK(mbedtls_md_hmac_update(&hmac_ctx, req->app_id, sizeof(req->app_id)));
        MCHECK(mbedtls_md_hmac_update(
            &hmac_ctx, req->key_handle.nonce, sizeof(req->key_handle.nonce)));
        MCHECK(mbedtls_md_hmac_finish(&hmac_ctx, priv_key));

        MCHECK(mbedtls_md_hmac_reset(&hmac_ctx));

        // Generate and verify private key handle
        MCHECK(mbedtls_md_hmac_update(&hmac_ctx, priv_key, sizeof(priv_key)));
        MCHECK(mbedtls_md_hmac_update(&hmac_ctx, req->app_id, sizeof(req->app_id)));
        MCHECK(mbedtls_md_hmac_finish(&hmac_ctx, mac_control));

        mbedtls_md_free(&hmac_ctx);
    }

    if(memcmp(req->key_handle.hash, mac_control, sizeof(mac_control)) != 0) {
        FURI_LOG_W(TAG, "Wrong handle!");
        memcpy(&buf[0], state_wrong_data, 2);
        return 2;
    }

    if(req->p1 == U2fCheckOnly) { // Check-only: don't need to send full response
        memcpy(&buf[0], state_user_missing, 2);
        return 2;
    }

    // Sign hash
    u2f_ecc_sign(&U2F->group, priv_key, hash, signature);

    resp->user_present = flags;
    resp->counter = be_u2f_counter;
    uint8_t signature_len = u2f_der_encode_signature(resp->signature, signature);
    memcpy(resp->signature + signature_len, state_no_error, 2);

    U2F->counter++;
    FURI_LOG_D(TAG, "Counter: %lu", U2F->counter);
    u2f_data_cnt_write(U2F->counter);

    if(U2F->callback != NULL) U2F->callback(U2fNotifyAuthSuccess, U2F->context);

    return sizeof(U2fAuthResp) + signature_len + 2;
}

uint16_t u2f_msg_parse(U2fData* U2F, uint8_t* buf, uint16_t len) {
    furi_assert(U2F);
    if(!U2F->ready) return 0;
    if((buf[0] != 0x00) && (len < 5)) return 0;
    if(buf[1] == U2F_CMD_REGISTER) { // Register request
        return u2f_register(U2F, buf);

    } else if(buf[1] == U2F_CMD_AUTHENTICATE) { // Authenticate request
        return u2f_authenticate(U2F, buf);

    } else if(buf[1] == U2F_CMD_VERSION) { // Get U2F version string
        memcpy(&buf[0], ver_str, 6);
        memcpy(&buf[6], state_no_error, 2);
        return 8;
    } else {
        memcpy(&buf[0], state_not_supported, 2);
        return 2;
    }
    return 0;
}

void u2f_wink(U2fData* U2F) {
    if(U2F->callback != NULL) U2F->callback(U2fNotifyWink, U2F->context);
}

void u2f_set_state(U2fData* U2F, uint8_t state) {
    if(state == 0) {
        if(U2F->callback != NULL) U2F->callback(U2fNotifyDisconnect, U2F->context);
    } else {
        if(U2F->callback != NULL) U2F->callback(U2fNotifyConnect, U2F->context);
    }
    U2F->user_present = false;
}
