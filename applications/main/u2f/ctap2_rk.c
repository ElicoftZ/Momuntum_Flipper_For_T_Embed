#include "ctap2_rk.h"

#include <stdio.h>
#include <string.h>

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_random.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#include <mbedtls/sha256.h>

#define TAG "Ctap2Rk"

#define U2F_RK_DIR EXT_PATH("u2f/rk")

#define U2F_RK_FILE_TYPE "Flipper U2F Resident Credential"
#define U2F_RK_VERSION   1

#define U2F_RK_KEY_SLOT FURI_HAL_CRYPTO_ENCLAVE_UNIQUE_KEY_SLOT

#define U2F_RK_CONTROL_VAL 0x524B4331UL /* "RKC1" */

/* Filenames are 16 hex chars plus ".rk". */
#define U2F_RK_NAME_LEN 20
#define U2F_RK_PATH_LEN (sizeof(U2F_RK_DIR) + U2F_RK_NAME_LEN + 2)

/* On-card layout. Must stay a whole number of AES blocks so it encrypts
 * without padding, hence the explicit reserved field. */
typedef struct {
    uint8_t rp_id_hash[CTAP2_RPID_HASH_SIZE];
    uint8_t cred_id[CTAP2_CRED_ID_SIZE];
    uint8_t user_id[CTAP2_RK_USER_ID_MAX];
    uint8_t user_id_len;
    char user_name[CTAP2_RK_NAME_MAX + 1];
    char display_name[CTAP2_RK_NAME_MAX + 1];
    char rp_id[CTAP2_RK_NAME_MAX + 1];
    uint8_t reserved[8];
    uint32_t control;
} FURI_PACKED U2fRkData;
_Static_assert(sizeof(U2fRkData) % 16 == 0, "U2fRkData must be a whole number of AES blocks");

/* Deterministic in (rp, user), so re-registering the same account at the same
 * site lands on the same file and replaces it instead of piling up. */
static void ctap2_rk_filename(
    const uint8_t* rp_id_hash,
    const uint8_t* user_id,
    size_t user_id_len,
    char* out) {
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, rp_id_hash, CTAP2_RPID_HASH_SIZE);
    mbedtls_sha256_update(&ctx, user_id, user_id_len);
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    for(size_t i = 0; i < 8; i++) {
        snprintf(out + i * 2, 3, "%02X", digest[i]);
    }
    memcpy(out + 16, ".rk", 4);
}

static bool ctap2_rk_read_file(const char* path, U2fRkData* out) {
    bool state = false;
    uint8_t iv[16];
    uint8_t encrypted[sizeof(U2fRkData)];
    uint32_t version = 0;

    FuriString* filetype = furi_string_alloc();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);

    if(flipper_format_file_open_existing(ff, path)) {
        do {
            if(!flipper_format_read_header(ff, filetype, &version)) break;
            if(strcmp(furi_string_get_cstr(filetype), U2F_RK_FILE_TYPE) != 0 ||
               version != U2F_RK_VERSION)
                break;
            if(!flipper_format_read_hex(ff, "IV", iv, sizeof(iv))) break;
            if(!flipper_format_read_hex(ff, "Data", encrypted, sizeof(encrypted))) break;

            if(!furi_hal_crypto_enclave_load_key(U2F_RK_KEY_SLOT, iv)) break;
            memset(out, 0, sizeof(U2fRkData));
            bool ok = furi_hal_crypto_decrypt(encrypted, (uint8_t*)out, sizeof(U2fRkData));
            furi_hal_crypto_enclave_unload_key(U2F_RK_KEY_SLOT);

            if(!ok || out->control != U2F_RK_CONTROL_VAL) {
                memset(out, 0, sizeof(U2fRkData));
                break;
            }
            /* Strings come off the card; a corrupt record must not hand an
             * unterminated buffer to snprintf later. */
            out->user_name[CTAP2_RK_NAME_MAX] = '\0';
            out->display_name[CTAP2_RK_NAME_MAX] = '\0';
            out->rp_id[CTAP2_RK_NAME_MAX] = '\0';
            if(out->user_id_len > CTAP2_RK_USER_ID_MAX) out->user_id_len = CTAP2_RK_USER_ID_MAX;
            state = true;
        } while(0);
    }

    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
    furi_string_free(filetype);
    return state;
}

bool ctap2_rk_store(const Ctap2RkRecord* record) {
    furi_assert(record);

    U2fRkData data;
    memset(&data, 0, sizeof(data));
    memcpy(data.rp_id_hash, record->rp_id_hash, CTAP2_RPID_HASH_SIZE);
    memcpy(data.cred_id, record->cred_id, CTAP2_CRED_ID_SIZE);

    data.user_id_len = record->user_id_len;
    if(data.user_id_len > CTAP2_RK_USER_ID_MAX) data.user_id_len = CTAP2_RK_USER_ID_MAX;
    memcpy(data.user_id, record->user_id, data.user_id_len);

    memcpy(data.user_name, record->user_name, CTAP2_RK_NAME_MAX);
    memcpy(data.display_name, record->display_name, CTAP2_RK_NAME_MAX);
    memcpy(data.rp_id, record->rp_id, CTAP2_RK_NAME_MAX);
    data.control = U2F_RK_CONTROL_VAL;

    char name[U2F_RK_NAME_LEN + 1];
    ctap2_rk_filename(data.rp_id_hash, data.user_id, data.user_id_len, name);

    char path[U2F_RK_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", U2F_RK_DIR, name);

    uint8_t iv[16];
    uint8_t encrypted[sizeof(U2fRkData)];
    furi_hal_random_fill_buf(iv, sizeof(iv));

    if(!furi_hal_crypto_enclave_load_key(U2F_RK_KEY_SLOT, iv)) {
        FURI_LOG_E(TAG, "Unable to load encryption key");
        return false;
    }
    bool ok = furi_hal_crypto_encrypt((uint8_t*)&data, encrypted, sizeof(U2fRkData));
    furi_hal_crypto_enclave_unload_key(U2F_RK_KEY_SLOT);
    memset(&data, 0, sizeof(data));
    if(!ok) return false;

    bool state = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, U2F_RK_DIR);

    FlipperFormat* ff = flipper_format_file_alloc(storage);
    if(flipper_format_file_open_always(ff, path)) {
        do {
            if(!flipper_format_write_header_cstr(ff, U2F_RK_FILE_TYPE, U2F_RK_VERSION)) break;
            if(!flipper_format_write_hex(ff, "IV", iv, sizeof(iv))) break;
            if(!flipper_format_write_hex(ff, "Data", encrypted, sizeof(encrypted))) break;
            state = true;
        } while(0);
    }
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);

    if(!state) FURI_LOG_E(TAG, "Failed to write %s", path);
    return state;
}

/* One walk of the directory, shared by every query.
 *
 * `want_rp` NULL counts everything; otherwise only records for that rp are
 * considered. When `out` is set, the `index`-th match is returned. Returns the
 * number of matches seen. */
static size_t ctap2_rk_walk(
    const uint8_t* want_rp,
    size_t index,
    Ctap2RkRecord* out,
    bool* found) {
    if(found != NULL) *found = false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    size_t matches = 0;

    if(storage_dir_open(dir, U2F_RK_DIR)) {
        FileInfo info;
        char name[U2F_RK_NAME_LEN + 8];

        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;

            char path[U2F_RK_PATH_LEN + 8];
            snprintf(path, sizeof(path), "%s/%s", U2F_RK_DIR, name);

            U2fRkData data;
            if(!ctap2_rk_read_file(path, &data)) continue;

            bool matched = (want_rp == NULL) ||
                           (memcmp(data.rp_id_hash, want_rp, CTAP2_RPID_HASH_SIZE) == 0);
            if(matched) {
                if(out != NULL && found != NULL && !*found && matches == index) {
                    memcpy(out->rp_id_hash, data.rp_id_hash, CTAP2_RPID_HASH_SIZE);
                    memcpy(out->cred_id, data.cred_id, CTAP2_CRED_ID_SIZE);
                    memcpy(out->user_id, data.user_id, CTAP2_RK_USER_ID_MAX);
                    out->user_id_len = data.user_id_len;
                    memcpy(out->user_name, data.user_name, sizeof(out->user_name));
                    memcpy(out->display_name, data.display_name, sizeof(out->display_name));
                    memcpy(out->rp_id, data.rp_id, sizeof(out->rp_id));
                    *found = true;
                }
                matches++;
            }
            memset(&data, 0, sizeof(data));
        }
    }

    /* Must be closed even when the open failed. */
    storage_dir_close(dir);
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);

    return matches;
}

size_t ctap2_rk_count_for_rp(const uint8_t* rp_id_hash) {
    return ctap2_rk_walk(rp_id_hash, 0, NULL, NULL);
}

bool ctap2_rk_get_for_rp(const uint8_t* rp_id_hash, size_t index, Ctap2RkRecord* out) {
    bool found = false;
    ctap2_rk_walk(rp_id_hash, index, out, &found);
    return found;
}

bool ctap2_rk_get_any(size_t index, Ctap2RkRecord* out) {
    bool found = false;
    ctap2_rk_walk(NULL, index, out, &found);
    return found;
}

bool ctap2_rk_exists(const uint8_t* rp_id_hash, const uint8_t* user_id, size_t user_id_len) {
    /* Filenames are a deterministic hash of (rp, user), so this is a single
     * stat rather than a walk of the whole directory. */
    char name[U2F_RK_NAME_LEN + 1];
    if(user_id_len > CTAP2_RK_USER_ID_MAX) user_id_len = CTAP2_RK_USER_ID_MAX;
    ctap2_rk_filename(rp_id_hash, user_id, user_id_len, name);

    char path[U2F_RK_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", U2F_RK_DIR, name);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool exists = storage_file_exists(storage, path);
    furi_record_close(RECORD_STORAGE);
    return exists;
}

bool ctap2_rk_delete(const uint8_t* rp_id_hash, const uint8_t* user_id, size_t user_id_len) {
    char name[U2F_RK_NAME_LEN + 1];
    if(user_id_len > CTAP2_RK_USER_ID_MAX) user_id_len = CTAP2_RK_USER_ID_MAX;
    ctap2_rk_filename(rp_id_hash, user_id, user_id_len, name);

    char path[U2F_RK_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", U2F_RK_DIR, name);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FS_Error err = storage_common_remove(storage, path);
    furi_record_close(RECORD_STORAGE);

    /* Already gone counts as deleted: the list is rebuilt from the directory
     * either way, and a second attempt must not report a failure. */
    bool state = (err == FSE_OK) || (err == FSE_NOT_EXIST);
    if(!state) FURI_LOG_E(TAG, "Failed to remove %s", path);
    return state;
}

size_t ctap2_rk_count_all(void) {
    return ctap2_rk_walk(NULL, 0, NULL, NULL);
}

void ctap2_rk_wipe(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);

    /* Collect first, delete after: removing entries while the directory
     * handle is walking it is not something FatFs promises to survive. */
    char victims[CTAP2_RK_MAX_CREDENTIALS][U2F_RK_NAME_LEN + 8];
    size_t victim_count = 0;

    if(storage_dir_open(dir, U2F_RK_DIR)) {
        FileInfo info;
        char name[U2F_RK_NAME_LEN + 8];
        while(storage_dir_read(dir, &info, name, sizeof(name)) &&
              victim_count < CTAP2_RK_MAX_CREDENTIALS) {
            if(info.flags & FSF_DIRECTORY) continue;
            strncpy(victims[victim_count], name, sizeof(victims[0]) - 1);
            victims[victim_count][sizeof(victims[0]) - 1] = '\0';
            victim_count++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);

    for(size_t i = 0; i < victim_count; i++) {
        char path[U2F_RK_PATH_LEN + 8];
        snprintf(path, sizeof(path), "%s/%s", U2F_RK_DIR, victims[i]);
        storage_common_remove(storage, path);
    }

    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I(TAG, "Wiped %u resident credentials", (unsigned)victim_count);
}
