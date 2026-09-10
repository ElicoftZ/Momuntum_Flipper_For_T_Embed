#include "furi_hal_crypto.h"

#include <string.h>

#include <core/common_defines.h>
#include <furi.h>

#include "furi_hal_random.h"

#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_hmac.h>
#include <esp_mac.h>
#include <sdkconfig.h>
#include <mbedtls/aes.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>

#define TAG "FuriHalCrypto"

/* The STM32 firmware this port follows keeps keys in a secure enclave split into
 * a factory region (slots 1-10, provisioned by Flipper and identical on every
 * device) and a per-device region (slots 12-100). The ESP32-S3 has no equivalent
 * key store. It does have an eFuse HMAC block, but burning one is permanent, so
 * this port does not.
 *
 * UNIQUE slot (11) -- real AES-256-CBC through mbedTLS. Its key is DERIVED,
 * from one of two sources tried in this order:
 *
 * 1. eFuse HMAC. If any eFuse key block carries the HMAC_UP purpose, the key is
 *    esp_hmac_calculate() over a fixed domain string. That block is write-only:
 *    software can never read it back, only ask the peripheral to use it. This is
 *    real secrecy -- dumping the flash and the SD card together is not enough.
 *
 *    The firmware NEVER burns a real key block. Burning is permanent, so it stays
 *    a deliberate act by the user:
 *      espefuse.py --port COMx burn_key BLOCK_KEY0 hmac_key.bin HMAC_UP
 *    Once burned, the branch below picks it up on the next boot with no config
 *    change. It also invalidates existing U2F data files, which were sealed with
 *    the fallback key.
 *
 * 2. Fallback: SHA-256 over the same domain string and the board's eFuse MAC.
 *    Device-unique, and deterministic so it survives a merged flash -- which
 *    matters here, because a merged image pads 0xFF over `nvs`, so a key stored
 *    in NVS would be destroyed on every reflash and take every U2F registration
 *    with it. The tradeoff is worth stating plainly: the MAC is broadcast by WiFi
 *    and BLE, so this is device BINDING, not secrecy. Someone holding both the SD
 *    card and the MAC can reproduce the key. That is what source 1 fixes.
 *
 * CONFIG_EFUSE_VIRTUAL redirects eFuse reads and writes to RAM, so the detection
 * and provisioning code can be exercised with nothing burned. Be clear about what
 * that proves: the HMAC peripheral reads the PHYSICAL key block, not the RAM
 * shadow, so esp_hmac_calculate() still fails and the fallback is what actually
 * runs. Virtual mode validates the plumbing, not the HMAC output.
 *
 * FACTORY slots -- pass through unchanged, exactly as this file did before the
 * unique slot became real. Flipper's factory keys are not available to any third
 * party, so nothing here can decrypt data sealed with them. lib/subghz's
 * keystore asks for slot 1 on every boot and has always been fed a passthrough;
 * turning that into a hard failure would break SubGHz to no benefit. Callers
 * that genuinely need to know cannot proceed should check the cert/key type
 * themselves -- applications/main/u2f/u2f_data.c does exactly that.
 */

#define CRYPTO_KEY_SIZE_BITS 256
#define CRYPTO_IV_SIZE       16
#define CRYPTO_BLOCK_SIZE    16

#define CRYPTO_UNIQUE_KEY_DOMAIN "Flipper Zero ESP32 port furi_hal_crypto unique key v1"

typedef enum {
    CryptoModeUnloaded,
    CryptoModePassthrough,
    CryptoModeAes,
} CryptoMode;

typedef struct {
    CryptoMode mode;
    uint8_t iv[CRYPTO_IV_SIZE];
    mbedtls_aes_context enc;
    mbedtls_aes_context dec;
} FuriHalCrypto;

static FuriHalCrypto furi_hal_crypto = {0};

static void crypto_aes_reset(void) {
    mbedtls_aes_free(&furi_hal_crypto.enc);
    mbedtls_aes_free(&furi_hal_crypto.dec);
    mbedtls_aes_init(&furi_hal_crypto.enc);
    mbedtls_aes_init(&furi_hal_crypto.dec);
    mbedtls_platform_zeroize(furi_hal_crypto.iv, sizeof(furi_hal_crypto.iv));
}

#if CONFIG_EFUSE_VIRTUAL
/* Virtual eFuses only: provision a key block so the HMAC_UP detection path below
 * has something to find. Writes land in the RAM shadow, never in silicon. */
static void crypto_provision_virtual_hmac_key(void) {
    esp_efuse_block_t block;
    if(esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &block)) {
        return;
    }

    uint8_t key[32];
    furi_hal_random_fill_buf(key, sizeof(key));
    esp_err_t err =
        esp_efuse_write_key(EFUSE_BLK_KEY0, ESP_EFUSE_KEY_PURPOSE_HMAC_UP, key, sizeof(key));
    mbedtls_platform_zeroize(key, sizeof(key));

    FURI_LOG_I(
        TAG,
        "Virtual eFuse HMAC key provisioned into BLOCK_KEY0: %s",
        (err == ESP_OK) ? "ok" : esp_err_to_name(err));
}
#endif

/* Preferred source: a write-only eFuse key block used through the HMAC
 * peripheral. Returns false when no block carries HMAC_UP, or when the
 * peripheral refuses it -- which is exactly what happens under virtual eFuses. */
static bool crypto_derive_from_hmac(uint8_t key[32]) {
    for(int id = 0; id < HMAC_KEY_MAX; id++) {
        esp_efuse_block_t block = (esp_efuse_block_t)(EFUSE_BLK_KEY0 + id);
        if(esp_efuse_get_key_purpose(block) != ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
            continue;
        }

        esp_err_t err = esp_hmac_calculate(
            (hmac_key_id_t)id,
            CRYPTO_UNIQUE_KEY_DOMAIN,
            strlen(CRYPTO_UNIQUE_KEY_DOMAIN),
            key);
        if(err == ESP_OK) {
            return true;
        }

        FURI_LOG_W(
            TAG,
            "eFuse key block %d claims HMAC_UP but the peripheral refused it: %s",
            id,
            esp_err_to_name(err));
    }
    return false;
}

/* The 128-bit factory ID in eFuse BLK2. Readable with nothing burned, and unlike
 * the MAC it is NEVER TRANSMITTED -- the MAC goes out in every WiFi and BLE
 * frame, so a MAC-only key can be reproduced by anyone who sniffed it and later
 * gets the SD card. Requiring the unique ID means they need the board itself.
 *
 * It is "optional": blank on some chips. Returns false when all-zero so the
 * caller falls back to the MAC-only derivation, which keeps the key byte-identical
 * to the pre-2026-08-23 one on such chips (no silent re-enrolment for them). */
static bool crypto_read_unique_id(uint8_t uid[16]) {
    if(esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, uid, 128) != ESP_OK) {
        return false;
    }

    for(size_t i = 0; i < 16; i++) {
        if(uid[i] != 0) return true;
    }
    return false;
}

static bool crypto_derive_from_chip(uint8_t key[32]) {
    uint8_t mac[6];
    if(esp_efuse_mac_get_default(mac) != ESP_OK) {
        FURI_LOG_E(TAG, "Unable to read eFuse MAC");
        return false;
    }

    uint8_t uid[16];
    const bool have_uid = crypto_read_unique_id(uid);

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);

    bool ok =
        (mbedtls_sha256_starts(&ctx, 0) == 0) &&
        (mbedtls_sha256_update(
             &ctx, (const uint8_t*)CRYPTO_UNIQUE_KEY_DOMAIN, strlen(CRYPTO_UNIQUE_KEY_DOMAIN)) ==
         0);

    /* Order matters and must never change: it defines the key, and any change
     * invalidates every credential derived from it. */
    if(ok && have_uid) {
        ok = mbedtls_sha256_update(&ctx, uid, sizeof(uid)) == 0;
    }
    if(ok) {
        ok = (mbedtls_sha256_update(&ctx, mac, sizeof(mac)) == 0) &&
             (mbedtls_sha256_finish(&ctx, key) == 0);
    }

    mbedtls_sha256_free(&ctx);
    mbedtls_platform_zeroize(mac, sizeof(mac));
    mbedtls_platform_zeroize(uid, sizeof(uid));

    if(ok && !have_uid) {
        FURI_LOG_W(TAG, "OPTIONAL_UNIQUE_ID is blank; key derived from the MAC alone");
    }

    return ok;
}

/* Compatibility derivation used before OPTIONAL_UNIQUE_ID was added. Keeping
 * it read-only lets U2F open an existing SD card once and reseal its key with
 * the stronger current derivation instead of reporting a certificate error. */
static bool crypto_derive_from_chip_legacy(uint8_t key[32]) {
    uint8_t mac[6];
    if(esp_efuse_mac_get_default(mac) != ESP_OK) return false;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    const bool ok =
        (mbedtls_sha256_starts(&ctx, 0) == 0) &&
        (mbedtls_sha256_update(
             &ctx, (const uint8_t*)CRYPTO_UNIQUE_KEY_DOMAIN, strlen(CRYPTO_UNIQUE_KEY_DOMAIN)) ==
         0) &&
        (mbedtls_sha256_update(&ctx, mac, sizeof(mac)) == 0) &&
        (mbedtls_sha256_finish(&ctx, key) == 0);
    mbedtls_sha256_free(&ctx);
    mbedtls_platform_zeroize(mac, sizeof(mac));
    return ok;
}

static bool crypto_derive_unique_key(uint8_t key[32]) {
#if CONFIG_EFUSE_VIRTUAL
    crypto_provision_virtual_hmac_key();
#endif

    /* Logged once: which source is in use decides whether existing U2F data files
     * still decrypt, so it has to be visible without guessing. */
    static bool reported = false;

    if(crypto_derive_from_hmac(key)) {
        if(!reported) {
            FURI_LOG_I(TAG, "Unique key source: eFuse HMAC key block");
            reported = true;
        }
        return true;
    }

    if(crypto_derive_from_chip(key)) {
        if(!reported) {
            FURI_LOG_I(TAG, "Unique key source: eFuse chip ID (no HMAC_UP key block burned)");
            reported = true;
        }
        return true;
    }

    return false;
}

void furi_hal_crypto_init(void) {
    memset(&furi_hal_crypto, 0, sizeof(furi_hal_crypto));
    mbedtls_aes_init(&furi_hal_crypto.enc);
    mbedtls_aes_init(&furi_hal_crypto.dec);
}

bool furi_hal_crypto_enclave_verify(uint8_t* keys_nb, uint8_t* valid_keys_nb) {
    if(keys_nb) {
        *keys_nb = 1;
    }
    if(valid_keys_nb) {
        *valid_keys_nb = 1;
    }
    return true;
}

bool furi_hal_crypto_enclave_ensure_key(uint8_t key_slot) {
    /* Nothing to provision: the unique key is derived on demand. */
    return key_slot <= FURI_HAL_CRYPTO_ENCLAVE_UNIQUE_KEY_SLOT;
}

bool furi_hal_crypto_enclave_store_key(FuriHalCryptoKey* key, uint8_t* slot) {
    if(!key || !slot) {
        return false;
    }

    *slot = FURI_HAL_CRYPTO_ENCLAVE_UNIQUE_KEY_SLOT;
    return true;
}

bool furi_hal_crypto_enclave_load_key(uint8_t slot, const uint8_t* iv) {
    if(slot != FURI_HAL_CRYPTO_ENCLAVE_UNIQUE_KEY_SLOT) {
        /* Factory slot: no key exists here, so encrypt/decrypt copy. See the
         * note at the top of this file. */
        UNUSED(iv);
        crypto_aes_reset();
        furi_hal_crypto.mode = CryptoModePassthrough;
        return true;
    }

    uint8_t key[32];
    if(!crypto_derive_unique_key(key)) {
        return false;
    }

    bool loaded = furi_hal_crypto_load_key(key, iv);
    mbedtls_platform_zeroize(key, sizeof(key));
    return loaded;
}

bool furi_hal_crypto_enclave_load_legacy_key(uint8_t slot, const uint8_t* iv) {
    if(slot != FURI_HAL_CRYPTO_ENCLAVE_UNIQUE_KEY_SLOT || !iv) return false;

    uint8_t key[32];
    if(!crypto_derive_from_chip_legacy(key)) return false;
    const bool loaded = furi_hal_crypto_load_key(key, iv);
    mbedtls_platform_zeroize(key, sizeof(key));
    return loaded;
}

bool furi_hal_crypto_enclave_unload_key(uint8_t slot) {
    UNUSED(slot);
    return furi_hal_crypto_unload_key();
}

bool furi_hal_crypto_load_key(const uint8_t* key, const uint8_t* iv) {
    if(!key || !iv) {
        return false;
    }

    /* Replace rather than refuse a second load. Callers in this tree return
     * early on an encrypt/decrypt failure without unloading, and refusing would
     * wedge the module until reboot. */
    crypto_aes_reset();
    furi_hal_crypto.mode = CryptoModeUnloaded;

    if(mbedtls_aes_setkey_enc(&furi_hal_crypto.enc, key, CRYPTO_KEY_SIZE_BITS) != 0 ||
       mbedtls_aes_setkey_dec(&furi_hal_crypto.dec, key, CRYPTO_KEY_SIZE_BITS) != 0) {
        FURI_LOG_E(TAG, "AES key schedule failed");
        crypto_aes_reset();
        return false;
    }

    memcpy(furi_hal_crypto.iv, iv, CRYPTO_IV_SIZE);
    furi_hal_crypto.mode = CryptoModeAes;
    return true;
}

bool furi_hal_crypto_unload_key(void) {
    crypto_aes_reset();
    furi_hal_crypto.mode = CryptoModeUnloaded;
    return true;
}

/* The IV chains across calls within one load, matching the STM32 CRYP peripheral
 * this replaces: it is initialised once at load and keeps its state until the key
 * is unloaded. */
static bool crypto_crypt(int operation, const uint8_t* input, uint8_t* output, size_t size) {
    if(!input || !output) {
        return false;
    }

    if(furi_hal_crypto.mode == CryptoModePassthrough) {
        memcpy(output, input, size);
        return true;
    }

    if(furi_hal_crypto.mode != CryptoModeAes) {
        FURI_LOG_E(TAG, "No key loaded");
        return false;
    }

    if(size == 0 || (size % CRYPTO_BLOCK_SIZE) != 0) {
        FURI_LOG_E(TAG, "Size %u is not a multiple of the AES block size", (unsigned)size);
        return false;
    }

    mbedtls_aes_context* ctx =
        (operation == MBEDTLS_AES_ENCRYPT) ? &furi_hal_crypto.enc : &furi_hal_crypto.dec;
    return mbedtls_aes_crypt_cbc(ctx, operation, size, furi_hal_crypto.iv, input, output) == 0;
}

bool furi_hal_crypto_encrypt(const uint8_t* input, uint8_t* output, size_t size) {
    return crypto_crypt(MBEDTLS_AES_ENCRYPT, input, output, size);
}

bool furi_hal_crypto_decrypt(const uint8_t* input, uint8_t* output, size_t size) {
    return crypto_crypt(MBEDTLS_AES_DECRYPT, input, output, size);
}
