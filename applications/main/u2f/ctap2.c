#include "ctap2.h"
#include "ctap2_crypto.h"
#include "ctap2_pin.h"
#include "ctap2_rk.h"
#include "fido_cbor.h"
#include "u2f_data.h"
#include "u2f_ecc.h"

#include <string.h>

#include <furi.h>
#include <furi_hal_random.h>

#include <mbedtls/sha256.h>

#define TAG "Ctap2"

/* authData is at most rpIdHash(32) + flags(1) + count(4) + aaguid(16) +
 * credIdLen(2) + credId(64) + a ~77-byte COSE key. 320 leaves headroom without
 * being worth measuring precisely. */
#define CTAP2_AUTH_DATA_MAX 320

#define CTAP2_CLIENT_DATA_HASH_SIZE 32

/* makeCredential request map keys (CTAP 2.1 §6.1). */
#define MC_KEY_CLIENT_DATA_HASH 1
#define MC_KEY_RP               2
#define MC_KEY_USER             3
#define MC_KEY_PUB_KEY_PARAMS   4
#define MC_KEY_EXCLUDE_LIST     5
#define MC_KEY_EXTENSIONS       6
#define MC_KEY_OPTIONS          7
#define MC_KEY_PIN_AUTH         8
#define MC_KEY_PIN_PROTOCOL     9

/* getAssertion request map keys (CTAP 2.1 §6.2). */
#define GA_KEY_RP_ID            1
#define GA_KEY_CLIENT_DATA_HASH 2
#define GA_KEY_ALLOW_LIST       3
#define GA_KEY_EXTENSIONS       4
#define GA_KEY_OPTIONS          5
#define GA_KEY_PIN_AUTH         6
#define GA_KEY_PIN_PROTOCOL     7

struct Ctap2 {
    U2fData* u2f;
    Ctap2Pin* pin;
    Ctap2PresenceFn presence_callback;
    void* presence_context;

    uint8_t auth_data[CTAP2_AUTH_DATA_MAX];

    uint8_t last_command;
    uint8_t last_status;

    /* An in-progress getAssertion sequence. A site with several accounts gets
     * the first credential from getAssertion and the rest from repeated
     * getNextAssertion calls, so the walk has to survive between commands. */
    struct {
        bool active;
        uint8_t rp_id_hash[CTAP2_RPID_HASH_SIZE];
        uint8_t client_data_hash[CTAP2_CLIENT_DATA_HASH_SIZE];
        size_t index;
        size_t count;
        uint8_t flags;
    } assertion;
};

Ctap2* ctap2_alloc(U2fData* u2f) {
    furi_assert(u2f);
    Ctap2* instance = calloc(1, sizeof(Ctap2));
    instance->u2f = u2f;
    instance->pin = ctap2_pin_alloc();
    return instance;
}

void ctap2_free(Ctap2* instance) {
    furi_assert(instance);
    ctap2_pin_free(instance->pin);
    /* Holds derived key material in auth_data only transiently, but wiping is
     * free at this point and the struct outlives individual ceremonies. */
    memset(instance, 0, sizeof(Ctap2));
    free(instance);
}

void ctap2_set_presence_callback(Ctap2* instance, Ctap2PresenceFn callback, void* context) {
    furi_assert(instance);
    instance->presence_callback = callback;
    instance->presence_context = context;
}

uint8_t ctap2_last_command(const Ctap2* instance) {
    return instance->last_command;
}

uint8_t ctap2_last_status(const Ctap2* instance) {
    return instance->last_status;
}

/* ------------------------------------------------------------- helpers */

/* Copy a non-NUL-terminated CBOR text string into a fixed buffer, truncating
 * rather than failing. */
static void ctap2_copy_truncated(char* dest, size_t dest_size, const char* src, size_t src_len) {
    if(src == NULL || src_len == 0) {
        dest[0] = 0;
        return;
    }
    size_t len = (src_len < dest_size - 1) ? src_len : dest_size - 1;
    memcpy(dest, src, len);
    dest[len] = 0;
}

static void ctap2_sha256(const uint8_t* data, size_t len, uint8_t* out) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

/* Every CTAP2 signature is over authData ‖ clientDataHash. */
static void ctap2_sign_payload(
    Ctap2* instance,
    const uint8_t* private_key,
    const uint8_t* auth_data,
    size_t auth_data_len,
    const uint8_t* client_data_hash,
    uint8_t* der_out,
    uint8_t* der_len_out) {
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, auth_data, auth_data_len);
    mbedtls_sha256_update(&ctx, client_data_hash, CTAP2_CLIENT_DATA_HASH_SIZE);
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    uint8_t raw_signature[U2F_EC_BIGNUM_SIZE * 2];
    u2f_ecc_sign(u2f_get_group(instance->u2f), private_key, digest, raw_signature);
    *der_len_out = u2f_der_encode_signature(der_out, raw_signature);
}

static uint8_t ctap2_ask_user(Ctap2* instance, U2fNotifyEvent prompt) {
    if(instance->presence_callback == NULL) return CTAP2_ERR_OPERATION_DENIED;

    switch(instance->presence_callback(instance->presence_context, prompt)) {
    case Ctap2PresenceGranted:
        return CTAP2_OK;
    case Ctap2PresenceCancelled:
        return CTAP2_ERR_KEEPALIVE_CANCEL;
    case Ctap2PresenceTimeout:
    default:
        return CTAP2_ERR_USER_ACTION_TIMEOUT;
    }
}

/* Read a {"id": ..., "type": ...} credential descriptor, returning the id.
 * Shared by excludeList and allowList, which have the same shape. */
static bool ctap2_read_credential_descriptor(
    CborReader* r,
    const uint8_t** id_out,
    size_t* id_len_out) {
    size_t fields = 0;
    if(!cbor_r_map(r, &fields)) return false;

    bool found = false;
    for(size_t i = 0; i < fields; i++) {
        const char* name = NULL;
        size_t name_len = 0;
        if(!cbor_r_tstr(r, &name, &name_len)) return false;

        if(cbor_r_tstr_equals(name, name_len, "id")) {
            if(!cbor_r_bstr(r, id_out, id_len_out)) return false;
            found = true;
        } else {
            /* "type", and anything a newer client adds. */
            if(!cbor_r_skip(r)) return false;
        }
    }
    return found;
}

/* Options maps are {"rk": bool, "up": bool, "uv": bool}. Absent keys keep the
 * caller's defaults, which differ per command, so they are passed in. */
static bool ctap2_read_options(CborReader* r, bool* rk, bool* up, bool* uv) {
    size_t count = 0;
    if(!cbor_r_map(r, &count)) return false;

    for(size_t i = 0; i < count; i++) {
        const char* name = NULL;
        size_t name_len = 0;
        if(!cbor_r_tstr(r, &name, &name_len)) return false;

        bool* target = NULL;
        if(cbor_r_tstr_equals(name, name_len, "rk")) {
            target = rk;
        } else if(cbor_r_tstr_equals(name, name_len, "up")) {
            target = up;
        } else if(cbor_r_tstr_equals(name, name_len, "uv")) {
            target = uv;
        }

        if(target != NULL) {
            if(!cbor_r_bool(r, target)) return false;
        } else if(!cbor_r_skip(r)) {
            return false;
        }
    }
    return true;
}

/* ----------------------------------------------------------- getInfo */

static size_t ctap2_get_info(Ctap2* instance, uint8_t* resp, size_t resp_cap) {
    UNUSED(instance);

    CborWriter w;
    cbor_w_init(&w, resp + 1, resp_cap - 1);

    const bool pin_set = ctap2_pin_is_set();

    /* Keys must appear in ascending order. Key 6 (pinUvAuthProtocols) only
     * makes sense alongside the clientPin option, so both come and go with
     * whether a PIN is set. */
    cbor_w_map(&w, pin_set ? 9 : 8);

    /* 1: versions. U2F_V2 stays advertised -- the CTAP1 path still works and
     * dropping it would break sites that only speak U2F. */
    cbor_w_uint(&w, 1);
    cbor_w_array(&w, 2);
    cbor_w_tstr(&w, "U2F_V2");
    cbor_w_tstr(&w, "FIDO_2_0");

    /* 3: aaguid */
    cbor_w_uint(&w, 3);
    cbor_w_bstr(&w, ctap2_aaguid, CTAP2_AAGUID_SIZE);

    /* 4: options. Text keys sort by length then bytewise, so this order --
     * rk, up, plat, clientPin -- is the canonical one.
     *
     * "uv" is deliberately absent: it means a BUILT-IN verification method
     * (biometric, on-device passcode), not a PIN. Advertising it without one
     * makes clients skip the PIN flow and then fail.
     *
     * "clientPin" is reported only once a PIN exists. The three states are
     * distinct in the spec: true is "set, ask for it", false is "supported,
     * none set yet", and ABSENT is "this device does not take a PIN at all".
     * False is what makes a site demand the user create one before it will
     * enrol the key -- which is exactly what happens on Google with a fresh
     * board. Absent leaves the PIN where this app puts it, in Settings, and
     * the moment one is set this turns into true and the host asks for it. */
    cbor_w_uint(&w, 4);
    cbor_w_map(&w, pin_set ? 4 : 3);
    cbor_w_tstr(&w, "rk");
    cbor_w_bool(&w, true); /* discoverable credentials, stored on the card */
    cbor_w_tstr(&w, "up");
    cbor_w_bool(&w, true);
    cbor_w_tstr(&w, "plat");
    cbor_w_bool(&w, false); /* removable, not a platform authenticator */
    if(pin_set) {
        cbor_w_tstr(&w, "clientPin");
        cbor_w_bool(&w, true);
    }

    /* 5: maxMsgSize */
    cbor_w_uint(&w, 5);
    cbor_w_uint(&w, CTAP2_MAX_MSG_SIZE);

    /* 6: pinUvAuthProtocols. v1 only -- v2 needs HKDF, which is off in
     * sdkconfig, and every client this device will meet supports v1. Omitted
     * with the clientPin option, so a PIN-less device does not look like it is
     * half offering one. */
    if(pin_set) {
        cbor_w_uint(&w, 6);
        cbor_w_array(&w, 1);
        cbor_w_uint(&w, 1);
    }

    /* 7, 8: bounds on what a client may send us. */
    cbor_w_uint(&w, 7);
    cbor_w_uint(&w, 8);
    cbor_w_uint(&w, 8);
    cbor_w_uint(&w, CTAP2_CRED_ID_SIZE);

    /* 9: transports */
    cbor_w_uint(&w, 9);
    cbor_w_array(&w, 1);
    cbor_w_tstr(&w, "usb");

    /* 10: algorithms */
    cbor_w_uint(&w, 10);
    cbor_w_array(&w, 1);
    cbor_w_map(&w, 2);
    cbor_w_tstr(&w, "alg");
    cbor_w_int(&w, -7); /* ES256 */
    cbor_w_tstr(&w, "type");
    cbor_w_tstr(&w, "public-key");

    size_t len = cbor_w_finish(&w);
    if(len == 0) {
        resp[0] = CTAP1_ERR_OTHER;
        return 1;
    }

    resp[0] = CTAP2_OK;
    return len + 1;
}

/* ---------------------------------------------------- makeCredential */

typedef struct {
    const uint8_t* client_data_hash;
    const char* rp_id;
    size_t rp_id_len;
    bool has_es256;
    bool option_rk;
    bool option_uv;
    const uint8_t* pin_auth;
    size_t pin_auth_len;
    bool has_pin_auth;

    /* Only needed for a discoverable credential, where the device has to be
     * able to name the account later without the site telling it. */
    const uint8_t* user_id;
    size_t user_id_len;
    const char* user_name;
    size_t user_name_len;
    const char* display_name;
    size_t display_name_len;

    /* Position of excludeList, revisited after user presence so a probing host
     * cannot enumerate registrations without a button press. */
    const uint8_t* exclude_list;
    size_t exclude_list_len;
} Ctap2MakeCredentialReq;

static uint8_t ctap2_parse_make_credential(
    const uint8_t* req,
    size_t req_len,
    Ctap2MakeCredentialReq* out) {
    CborReader r;
    cbor_r_init(&r, req, req_len);

    size_t entries = 0;
    if(!cbor_r_map(&r, &entries)) return CTAP2_ERR_INVALID_CBOR;

    memset(out, 0, sizeof(*out));

    for(size_t i = 0; i < entries; i++) {
        uint64_t key = 0;
        if(!cbor_r_uint(&r, &key)) return CTAP2_ERR_INVALID_CBOR;

        switch(key) {
        case MC_KEY_CLIENT_DATA_HASH: {
            size_t len = 0;
            if(!cbor_r_bstr(&r, &out->client_data_hash, &len)) return CTAP2_ERR_INVALID_CBOR;
            if(len != CTAP2_CLIENT_DATA_HASH_SIZE) return CTAP1_ERR_INVALID_LENGTH;
            break;
        }

        case MC_KEY_RP: {
            size_t fields = 0;
            if(!cbor_r_map(&r, &fields)) return CTAP2_ERR_INVALID_CBOR;
            for(size_t f = 0; f < fields; f++) {
                const char* name = NULL;
                size_t name_len = 0;
                if(!cbor_r_tstr(&r, &name, &name_len)) return CTAP2_ERR_INVALID_CBOR;
                if(cbor_r_tstr_equals(name, name_len, "id")) {
                    if(!cbor_r_tstr(&r, &out->rp_id, &out->rp_id_len))
                        return CTAP2_ERR_INVALID_CBOR;
                } else if(!cbor_r_skip(&r)) {
                    return CTAP2_ERR_INVALID_CBOR;
                }
            }
            break;
        }

        case MC_KEY_PUB_KEY_PARAMS: {
            size_t count = 0;
            if(!cbor_r_array(&r, &count)) return CTAP2_ERR_INVALID_CBOR;
            for(size_t p = 0; p < count; p++) {
                size_t fields = 0;
                if(!cbor_r_map(&r, &fields)) return CTAP2_ERR_INVALID_CBOR;
                int64_t alg = 0;
                bool alg_seen = false;
                bool is_public_key = false;
                for(size_t f = 0; f < fields; f++) {
                    const char* name = NULL;
                    size_t name_len = 0;
                    if(!cbor_r_tstr(&r, &name, &name_len)) return CTAP2_ERR_INVALID_CBOR;
                    if(cbor_r_tstr_equals(name, name_len, "alg")) {
                        if(!cbor_r_int(&r, &alg)) return CTAP2_ERR_INVALID_CBOR;
                        alg_seen = true;
                    } else if(cbor_r_tstr_equals(name, name_len, "type")) {
                        const char* type = NULL;
                        size_t type_len = 0;
                        if(!cbor_r_tstr(&r, &type, &type_len)) return CTAP2_ERR_INVALID_CBOR;
                        is_public_key = cbor_r_tstr_equals(type, type_len, "public-key");
                    } else if(!cbor_r_skip(&r)) {
                        return CTAP2_ERR_INVALID_CBOR;
                    }
                }
                /* Clients list several algorithms in preference order; taking
                 * the first ES256 entry rather than requiring it to be first
                 * is what lets an Ed25519-preferring client still work. */
                if(alg_seen && is_public_key && alg == -7) out->has_es256 = true;
            }
            break;
        }

        case MC_KEY_EXCLUDE_LIST: {
            /* Remember where it starts and step over it. Walking it here would
             * mean answering before the user has touched the device. */
            out->exclude_list = req + r.pos;
            size_t before = r.pos;
            if(!cbor_r_skip(&r)) return CTAP2_ERR_INVALID_CBOR;
            out->exclude_list_len = r.pos - before;
            break;
        }

        case MC_KEY_OPTIONS: {
            bool up_ignored = true;
            if(!ctap2_read_options(&r, &out->option_rk, &up_ignored, &out->option_uv))
                return CTAP2_ERR_INVALID_CBOR;
            break;
        }

        case MC_KEY_PIN_AUTH:
            if(!cbor_r_bstr(&r, &out->pin_auth, &out->pin_auth_len))
                return CTAP2_ERR_INVALID_CBOR;
            out->has_pin_auth = true;
            break;

        case MC_KEY_USER: {
            size_t fields = 0;
            if(!cbor_r_map(&r, &fields)) return CTAP2_ERR_INVALID_CBOR;
            for(size_t f = 0; f < fields; f++) {
                const char* name = NULL;
                size_t name_len = 0;
                if(!cbor_r_tstr(&r, &name, &name_len)) return CTAP2_ERR_INVALID_CBOR;

                if(cbor_r_tstr_equals(name, name_len, "id")) {
                    if(!cbor_r_bstr(&r, &out->user_id, &out->user_id_len))
                        return CTAP2_ERR_INVALID_CBOR;
                } else if(cbor_r_tstr_equals(name, name_len, "name")) {
                    if(!cbor_r_tstr(&r, &out->user_name, &out->user_name_len))
                        return CTAP2_ERR_INVALID_CBOR;
                } else if(cbor_r_tstr_equals(name, name_len, "displayName")) {
                    if(!cbor_r_tstr(&r, &out->display_name, &out->display_name_len))
                        return CTAP2_ERR_INVALID_CBOR;
                } else if(!cbor_r_skip(&r)) {
                    return CTAP2_ERR_INVALID_CBOR;
                }
            }
            break;
        }

        default:
            /* extensions, pinProtocol and anything newer. */
            if(!cbor_r_skip(&r)) return CTAP2_ERR_INVALID_CBOR;
            break;
        }
    }

    if(out->client_data_hash == NULL || out->rp_id == NULL) return CTAP2_ERR_MISSING_PARAMETER;
    if(!out->has_es256) return CTAP2_ERR_UNSUPPORTED_ALGORITHM;
    return CTAP2_OK;
}

/* True if any excludeList entry names a credential this device issued for this
 * rp -- i.e. the site already has a registration here. */
static bool ctap2_exclude_list_matches(
    Ctap2* instance,
    const Ctap2MakeCredentialReq* req,
    const uint8_t* rp_id_hash) {
    if(req->exclude_list == NULL) return false;

    CborReader r;
    cbor_r_init(&r, req->exclude_list, req->exclude_list_len);

    size_t count = 0;
    if(!cbor_r_array(&r, &count)) return false;

    for(size_t i = 0; i < count; i++) {
        const uint8_t* id = NULL;
        size_t id_len = 0;
        if(!ctap2_read_credential_descriptor(&r, &id, &id_len)) return false;

        uint8_t private_key[U2F_EC_KEY_SIZE];
        bool match = ctap2_open_credential_id(
            u2f_get_device_key(instance->u2f), rp_id_hash, id, id_len, private_key);
        memset(private_key, 0, sizeof(private_key));
        if(match) return true;
    }
    return false;
}

static size_t
    ctap2_make_credential(Ctap2* instance, const uint8_t* req, size_t req_len, uint8_t* resp, size_t resp_cap) {
    Ctap2MakeCredentialReq parsed;
    uint8_t status = ctap2_parse_make_credential(req, req_len, &parsed);
    if(status != CTAP2_OK) {
        resp[0] = status;
        return 1;
    }

    /* A discoverable credential has to name its account later, and only
     * user.id can do that -- the spec makes it mandatory for exactly this. */
    if(parsed.option_rk && (parsed.user_id == NULL || parsed.user_id_len == 0)) {
        resp[0] = CTAP2_ERR_MISSING_PARAMETER;
        return 1;
    }
    /* "uv" here asks for a BUILT-IN verification method. This device verifies
     * with a PIN, which the client requests through pinUvAuthParam instead. */
    if(parsed.option_uv) {
        resp[0] = CTAP2_ERR_UNSUPPORTED_OPTION;
        return 1;
    }

    /* This block is what makes the PIN actually bite. Once one is set, a
     * registration without proof of it is refused outright -- otherwise the
     * PIN would be a setting the host could simply decline to use. */
    uint8_t flags = CTAP2_FLAG_UP;
    if(parsed.has_pin_auth) {
        if(!ctap2_pin_is_set()) {
            resp[0] = CTAP2_ERR_PIN_NOT_SET;
            return 1;
        }
        if(!ctap2_pin_verify_auth_param(
               instance->pin,
               parsed.client_data_hash,
               CTAP2_CLIENT_DATA_HASH_SIZE,
               parsed.pin_auth,
               parsed.pin_auth_len)) {
            resp[0] = CTAP2_ERR_PIN_AUTH_INVALID;
            return 1;
        }
        flags |= CTAP2_FLAG_UV;
    } else if(ctap2_pin_is_set()) {
        resp[0] = CTAP2_ERR_PIN_REQUIRED;
        return 1;
    }

    uint8_t rp_id_hash[CTAP2_RPID_HASH_SIZE];
    ctap2_sha256((const uint8_t*)parsed.rp_id, parsed.rp_id_len, rp_id_hash);

    status = ctap2_ask_user(instance, U2fNotifyRegister);
    if(status != CTAP2_OK) {
        resp[0] = status;
        return 1;
    }

    /* Only now, with the user's consent recorded, is it safe to say whether a
     * credential for this site already exists. */
    if(ctap2_exclude_list_matches(instance, &parsed, rp_id_hash)) {
        resp[0] = CTAP2_ERR_CREDENTIAL_EXCLUDED;
        return 1;
    }

    uint8_t cred_id[CTAP2_CRED_ID_SIZE];
    uint8_t private_key[U2F_EC_KEY_SIZE];
    ctap2_make_credential_id(
        u2f_get_device_key(instance->u2f), rp_id_hash, cred_id, private_key);

    U2fPubKey pub_key;
    u2f_ecc_compute_public_key(u2f_get_group(instance->u2f), private_key, &pub_key);

    /* Written before the response is built: a credential the site is told
     * about but which was never stored would look registered and then fail
     * every subsequent login. */
    if(parsed.option_rk) {
        Ctap2RkRecord record;
        memset(&record, 0, sizeof(record));
        memcpy(record.rp_id_hash, rp_id_hash, CTAP2_RPID_HASH_SIZE);
        memcpy(record.cred_id, cred_id, CTAP2_CRED_ID_SIZE);

        record.user_id_len = (uint8_t)((parsed.user_id_len > CTAP2_RK_USER_ID_MAX) ?
                                           CTAP2_RK_USER_ID_MAX :
                                           parsed.user_id_len);
        memcpy(record.user_id, parsed.user_id, record.user_id_len);

        /* Names are display sugar and are simply truncated; the user handle
         * above is the part that has to survive intact. */
        ctap2_copy_truncated(
            record.user_name, sizeof(record.user_name), parsed.user_name, parsed.user_name_len);
        ctap2_copy_truncated(
            record.display_name,
            sizeof(record.display_name),
            parsed.display_name,
            parsed.display_name_len);
        ctap2_copy_truncated(record.rp_id, sizeof(record.rp_id), parsed.rp_id, parsed.rp_id_len);

        /* Only refuse when this is a NEW account: re-registering an existing
         * one replaces its record in place and needs no extra room. */
        if(ctap2_rk_count_all() >= CTAP2_RK_MAX_CREDENTIALS &&
           !ctap2_rk_exists(rp_id_hash, parsed.user_id, parsed.user_id_len)) {
            memset(private_key, 0, sizeof(private_key));
            resp[0] = CTAP2_ERR_KEY_STORE_FULL;
            return 1;
        }

        if(!ctap2_rk_store(&record)) {
            memset(&record, 0, sizeof(record));
            memset(private_key, 0, sizeof(private_key));
            resp[0] = CTAP2_ERR_KEY_STORE_FULL;
            return 1;
        }
        memset(&record, 0, sizeof(record));
    }

    uint32_t sign_count = u2f_bump_counter(instance->u2f);

    size_t auth_data_len = ctap2_build_auth_data(
        instance->auth_data,
        sizeof(instance->auth_data),
        rp_id_hash,
        flags,
        sign_count,
        cred_id,
        sizeof(cred_id),
        &pub_key);

    if(auth_data_len == 0) {
        memset(private_key, 0, sizeof(private_key));
        resp[0] = CTAP1_ERR_OTHER;
        return 1;
    }

    /* SELF ATTESTATION: signed with the freshly minted credential key, and no
     * x5c at all.
     *
     * The alternative is basic attestation with a certificate, and neither
     * form of it is honest here. One certificate shared by every build has its
     * private key in the source tree, so it proves nothing. A per-device
     * generated one proves nothing either -- nobody has signed it -- while
     * handing every relying party a stable identifier that correlates the same
     * device across unrelated sites, which is the exact problem FIDO's batch
     * attestation exists to avoid.
     *
     * Self attestation says only "the key that answered owns this credential",
     * which is all this device can truthfully claim, and it verifies anywhere
     * with nothing to provision on the card and nothing that can go stale. */
    uint8_t signature[80];
    uint8_t signature_len = 0;
    ctap2_sign_payload(
        instance,
        private_key,
        instance->auth_data,
        auth_data_len,
        parsed.client_data_hash,
        signature,
        &signature_len);

    memset(private_key, 0, sizeof(private_key));

    CborWriter w;
    cbor_w_init(&w, resp + 1, resp_cap - 1);

    cbor_w_map(&w, 3);

    cbor_w_uint(&w, 1); /* fmt */
    cbor_w_tstr(&w, "packed");

    cbor_w_uint(&w, 2); /* authData */
    cbor_w_bstr(&w, instance->auth_data, auth_data_len);

    /* attStmt -- "alg" < "sig" is canonical order. No x5c: its absence is
     * precisely what tells the verifier to check the signature against the
     * credential public key it just received, i.e. self attestation. */
    cbor_w_uint(&w, 3);
    cbor_w_map(&w, 2);
    cbor_w_tstr(&w, "alg");
    cbor_w_int(&w, -7);
    cbor_w_tstr(&w, "sig");
    cbor_w_bstr(&w, signature, signature_len);

    size_t len = cbor_w_finish(&w);
    if(len == 0) {
        resp[0] = CTAP2_ERR_REQUEST_TOO_LARGE;
        return 1;
    }

    resp[0] = CTAP2_OK;
    return len + 1;
}

/* ------------------------------------------------------ getAssertion */

/* Build and send one assertion. Shared by getAssertion and getNextAssertion,
 * which differ only in where the credential came from and whether the response
 * carries numberOfCredentials. */
static size_t ctap2_emit_assertion(
    Ctap2* instance,
    const uint8_t* rp_id_hash,
    const uint8_t* cred_id,
    const uint8_t* client_data_hash,
    uint8_t flags,
    const Ctap2RkRecord* rk,
    size_t number_of_credentials,
    uint8_t* resp,
    size_t resp_cap) {
    uint8_t private_key[U2F_EC_KEY_SIZE];
    if(!ctap2_open_credential_id(
           u2f_get_device_key(instance->u2f),
           rp_id_hash,
           cred_id,
           CTAP2_CRED_ID_SIZE,
           private_key)) {
        /* For a resident credential this means the device key has changed
         * since it was written -- a reset, or a different board. */
        memset(private_key, 0, sizeof(private_key));
        resp[0] = CTAP2_ERR_NO_CREDENTIALS;
        return 1;
    }

    uint32_t sign_count = u2f_bump_counter(instance->u2f);

    size_t auth_data_len = ctap2_build_auth_data(
        instance->auth_data,
        sizeof(instance->auth_data),
        rp_id_hash,
        flags,
        sign_count,
        NULL,
        0,
        NULL);

    if(auth_data_len == 0) {
        memset(private_key, 0, sizeof(private_key));
        resp[0] = CTAP1_ERR_OTHER;
        return 1;
    }

    uint8_t signature[80];
    uint8_t signature_len = 0;
    ctap2_sign_payload(
        instance,
        private_key,
        instance->auth_data,
        auth_data_len,
        client_data_hash,
        signature,
        &signature_len);
    memset(private_key, 0, sizeof(private_key));

    /* Keys 4 (user) and 5 (numberOfCredentials) are present only for
     * discoverable credentials, and 5 only on the first of several. */
    size_t map_entries = 3;
    if(rk != NULL) map_entries++;
    if(number_of_credentials > 1) map_entries++;

    CborWriter w;
    cbor_w_init(&w, resp + 1, resp_cap - 1);
    cbor_w_map(&w, map_entries);

    cbor_w_uint(&w, 1); /* credential -- "id" before "type", shorter key first */
    cbor_w_map(&w, 2);
    cbor_w_tstr(&w, "id");
    cbor_w_bstr(&w, cred_id, CTAP2_CRED_ID_SIZE);
    cbor_w_tstr(&w, "type");
    cbor_w_tstr(&w, "public-key");

    cbor_w_uint(&w, 2); /* authData */
    cbor_w_bstr(&w, instance->auth_data, auth_data_len);

    cbor_w_uint(&w, 3); /* signature */
    cbor_w_bstr(&w, signature, signature_len);

    if(rk != NULL) {
        /* The user handle is what lets a site log someone in without them
         * first typing a username -- the whole point of a passkey. Name and
         * displayName are optional, and omitted when blank rather than sent
         * as empty strings. */
        size_t user_entries = 1;
        if(rk->user_name[0] != 0) user_entries++;
        if(rk->display_name[0] != 0) user_entries++;

        cbor_w_uint(&w, 4);
        cbor_w_map(&w, user_entries);
        cbor_w_tstr(&w, "id");
        cbor_w_bstr(&w, rk->user_id, rk->user_id_len);
        if(rk->user_name[0] != 0) {
            cbor_w_tstr(&w, "name");
            cbor_w_tstr(&w, rk->user_name);
        }
        if(rk->display_name[0] != 0) {
            cbor_w_tstr(&w, "displayName");
            cbor_w_tstr(&w, rk->display_name);
        }
    }

    if(number_of_credentials > 1) {
        cbor_w_uint(&w, 5);
        cbor_w_uint(&w, number_of_credentials);
    }

    size_t len = cbor_w_finish(&w);
    if(len == 0) {
        resp[0] = CTAP2_ERR_REQUEST_TOO_LARGE;
        return 1;
    }

    u2f_notify(instance->u2f, U2fNotifyAuthSuccess);
    resp[0] = CTAP2_OK;
    return len + 1;
}

static size_t
    ctap2_get_assertion(Ctap2* instance, const uint8_t* req, size_t req_len, uint8_t* resp, size_t resp_cap) {
    CborReader r;
    cbor_r_init(&r, req, req_len);

    /* Any new getAssertion cancels a getNextAssertion sequence in progress. */
    instance->assertion.active = false;

    size_t entries = 0;
    if(!cbor_r_map(&r, &entries)) {
        resp[0] = CTAP2_ERR_INVALID_CBOR;
        return 1;
    }

    const char* rp_id = NULL;
    size_t rp_id_len = 0;
    const uint8_t* client_data_hash = NULL;
    const uint8_t* allow_list = NULL;
    size_t allow_list_len = 0;
    bool option_up = true; /* getAssertion defaults up to true */
    bool option_uv = false;
    bool option_rk_ignored = false;
    const uint8_t* pin_auth = NULL;
    size_t pin_auth_len = 0;
    bool has_pin_auth = false;

    for(size_t i = 0; i < entries; i++) {
        uint64_t key = 0;
        if(!cbor_r_uint(&r, &key)) {
            resp[0] = CTAP2_ERR_INVALID_CBOR;
            return 1;
        }

        bool ok = true;
        switch(key) {
        case GA_KEY_RP_ID:
            ok = cbor_r_tstr(&r, &rp_id, &rp_id_len);
            break;

        case GA_KEY_CLIENT_DATA_HASH: {
            size_t len = 0;
            ok = cbor_r_bstr(&r, &client_data_hash, &len);
            if(ok && len != CTAP2_CLIENT_DATA_HASH_SIZE) {
                resp[0] = CTAP1_ERR_INVALID_LENGTH;
                return 1;
            }
            break;
        }

        case GA_KEY_ALLOW_LIST: {
            allow_list = req + r.pos;
            size_t before = r.pos;
            ok = cbor_r_skip(&r);
            allow_list_len = r.pos - before;
            break;
        }

        case GA_KEY_OPTIONS:
            ok = ctap2_read_options(&r, &option_rk_ignored, &option_up, &option_uv);
            break;

        case GA_KEY_PIN_AUTH:
            ok = cbor_r_bstr(&r, &pin_auth, &pin_auth_len);
            has_pin_auth = true;
            break;

        default:
            ok = cbor_r_skip(&r);
            break;
        }

        if(!ok) {
            resp[0] = CTAP2_ERR_INVALID_CBOR;
            return 1;
        }
    }

    if(rp_id == NULL || client_data_hash == NULL) {
        resp[0] = CTAP2_ERR_MISSING_PARAMETER;
        return 1;
    }

    /* Unlike makeCredential, an assertion WITHOUT the PIN is legal -- it just
     * carries uv=0, and a relying party that asked for user verification will
     * reject it on those grounds. Refusing here instead would break every site
     * that only wants a second factor. */
    uint8_t uv_flag = 0;
    if(has_pin_auth) {
        if(!ctap2_pin_is_set()) {
            resp[0] = CTAP2_ERR_PIN_NOT_SET;
            return 1;
        }
        if(!ctap2_pin_verify_auth_param(
               instance->pin,
               client_data_hash,
               CTAP2_CLIENT_DATA_HASH_SIZE,
               pin_auth,
               pin_auth_len)) {
            resp[0] = CTAP2_ERR_PIN_AUTH_INVALID;
            return 1;
        }
        uv_flag = CTAP2_FLAG_UV;
    } else if(option_uv) {
        /* The client demanded verification but supplied no proof of the PIN,
         * which is this device's only means of providing it. */
        resp[0] = ctap2_pin_is_set() ? CTAP2_ERR_PIN_REQUIRED : CTAP2_ERR_UNSUPPORTED_OPTION;
        return 1;
    }

    uint8_t rp_id_hash[CTAP2_RPID_HASH_SIZE];
    ctap2_sha256((const uint8_t*)rp_id, rp_id_len, rp_id_hash);

    uint8_t cred_id[CTAP2_CRED_ID_SIZE];
    Ctap2RkRecord rk;
    bool resident = false;
    bool found = false;
    size_t rk_count = 0;

    if(allow_list != NULL) {
        /* The site named the credentials it already knows about. */
        CborReader list;
        cbor_r_init(&list, allow_list, allow_list_len);
        size_t count = 0;
        if(!cbor_r_array(&list, &count)) {
            resp[0] = CTAP2_ERR_INVALID_CBOR;
            return 1;
        }

        uint8_t private_key[U2F_EC_KEY_SIZE];
        for(size_t i = 0; i < count && !found; i++) {
            const uint8_t* id = NULL;
            size_t id_len = 0;
            if(!ctap2_read_credential_descriptor(&list, &id, &id_len)) break;

            if(ctap2_open_credential_id(
                   u2f_get_device_key(instance->u2f), rp_id_hash, id, id_len, private_key)) {
                memcpy(cred_id, id, CTAP2_CRED_ID_SIZE);
                found = true;
            }
        }
        memset(private_key, 0, sizeof(private_key));

    } else {
        /* No allowList: the site is asking who we hold for it, which only a
         * discoverable credential can answer. */
        rk_count = ctap2_rk_count_for_rp(rp_id_hash);
        if(rk_count > 0 && ctap2_rk_get_for_rp(rp_id_hash, 0, &rk)) {
            memcpy(cred_id, rk.cred_id, CTAP2_CRED_ID_SIZE);
            resident = true;
            found = true;
        }
    }

    if(!found) {
        resp[0] = CTAP2_ERR_NO_CREDENTIALS;
        return 1;
    }

    uint8_t flags = uv_flag;
    if(option_up) {
        uint8_t status = ctap2_ask_user(instance, U2fNotifyAuth);
        if(status != CTAP2_OK) {
            resp[0] = status;
            return 1;
        }
        flags |= CTAP2_FLAG_UP;
    }

    size_t len = ctap2_emit_assertion(
        instance,
        rp_id_hash,
        cred_id,
        client_data_hash,
        flags,
        resident ? &rk : NULL,
        resident ? rk_count : 0,
        resp,
        resp_cap);

    /* Arm getNextAssertion only when there is genuinely more to hand over. The
     * user presence just obtained covers the whole sequence; the spec does not
     * ask for a fresh press per credential. */
    if(resident && rk_count > 1 && resp[0] == CTAP2_OK) {
        instance->assertion.active = true;
        memcpy(instance->assertion.rp_id_hash, rp_id_hash, CTAP2_RPID_HASH_SIZE);
        memcpy(
            instance->assertion.client_data_hash,
            client_data_hash,
            CTAP2_CLIENT_DATA_HASH_SIZE);
        instance->assertion.index = 0;
        instance->assertion.count = rk_count;
        instance->assertion.flags = flags;
    }

    return len;
}

static size_t ctap2_get_next_assertion(Ctap2* instance, uint8_t* resp, size_t resp_cap) {
    if(!instance->assertion.active) {
        resp[0] = CTAP2_ERR_NOT_ALLOWED;
        return 1;
    }

    instance->assertion.index++;
    if(instance->assertion.index >= instance->assertion.count) {
        instance->assertion.active = false;
        resp[0] = CTAP2_ERR_NOT_ALLOWED;
        return 1;
    }

    Ctap2RkRecord rk;
    if(!ctap2_rk_get_for_rp(instance->assertion.rp_id_hash, instance->assertion.index, &rk)) {
        instance->assertion.active = false;
        resp[0] = CTAP2_ERR_NO_CREDENTIALS;
        return 1;
    }

    return ctap2_emit_assertion(
        instance,
        instance->assertion.rp_id_hash,
        rk.cred_id,
        instance->assertion.client_data_hash,
        instance->assertion.flags,
        &rk,
        0, /* numberOfCredentials appears only in the first response */
        resp,
        resp_cap);
}

/* ----------------------------------------------------------- reset */

/* Wipes the PIN and every credential this device can assert.
 *
 * The credentials go not by being deleted -- non-resident ones are not stored
 * anywhere to delete -- but because the device key they are all wrapped under
 * is regenerated. Nothing issued before this point can be opened again. */
static size_t ctap2_reset(Ctap2* instance, uint8_t* resp) {
    /* The spec permits requiring user confirmation instead of the 10-second
     * post-boot window, and that is the safer reading on a device someone can
     * pick up: a host must not be able to silently destroy every credential. */
    uint8_t status = ctap2_ask_user(instance, U2fNotifyRegister);
    if(status != CTAP2_OK) {
        resp[0] = status;
        return 1;
    }

    ctap2_pin_erase();
    ctap2_pin_revoke_token(instance->pin);
    ctap2_rk_wipe();
    instance->assertion.active = false;

    if(!u2f_regenerate_device_key(instance->u2f)) {
        resp[0] = CTAP1_ERR_OTHER;
        return 1;
    }

    FURI_LOG_I(TAG, "Authenticator reset: PIN cleared, device key regenerated");
    resp[0] = CTAP2_OK;
    return 1;
}

/* --------------------------------------------------------- dispatch */

size_t ctap2_request(
    Ctap2* instance,
    const uint8_t* req,
    size_t req_len,
    uint8_t* resp,
    size_t resp_cap) {
    furi_assert(instance);
    furi_assert(resp_cap >= 1);

    if(req_len < 1) {
        instance->last_command = 0;
        instance->last_status = CTAP1_ERR_INVALID_LENGTH;
        resp[0] = CTAP1_ERR_INVALID_LENGTH;
        return 1;
    }

    const uint8_t command = req[0];
    const uint8_t* payload = req + 1;
    const size_t payload_len = req_len - 1;
    instance->last_command = command;

    /* Files can vanish with the SD card between one command and the next, and
     * a missing attestation cert must surface as a status rather than as a
     * signature over uninitialised memory. */
    if(!u2f_data_check(false)) {
        u2f_notify(instance->u2f, U2fNotifyError);
        instance->last_status = CTAP1_ERR_OTHER;
        resp[0] = CTAP1_ERR_OTHER;
        return 1;
    }

    size_t len;
    switch(command) {
    case CTAP2_CMD_GET_INFO:
        len = ctap2_get_info(instance, resp, resp_cap);
        break;

    case CTAP2_CMD_MAKE_CREDENTIAL:
        len = ctap2_make_credential(instance, payload, payload_len, resp, resp_cap);
        break;

    case CTAP2_CMD_GET_ASSERTION:
        len = ctap2_get_assertion(instance, payload, payload_len, resp, resp_cap);
        break;

    case CTAP2_CMD_CLIENT_PIN:
        len = ctap2_pin_command(
            instance->pin, u2f_get_group(instance->u2f), payload, payload_len, resp, resp_cap);
        break;

    case CTAP2_CMD_GET_NEXT_ASSERTION:
        len = ctap2_get_next_assertion(instance, resp, resp_cap);
        break;

    case CTAP2_CMD_RESET:
        len = ctap2_reset(instance, resp);
        break;

    default:
        resp[0] = CTAP1_ERR_INVALID_COMMAND;
        len = 1;
        break;
    }

    instance->last_status = resp[0];
    FURI_LOG_D(TAG, "cmd=%02X status=%02X len=%u", command, resp[0], (unsigned)len);
    return len;
}
