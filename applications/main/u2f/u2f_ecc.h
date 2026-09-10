#pragma once

/* P-256 primitives shared by the CTAP1 (U2F) and CTAP2 (FIDO2) paths.
 *
 * These live in u2f.c, which owned them first. They are declared here rather
 * than copied into the CTAP2 sources: both paths sign with the same curve, the
 * same deterministic-ECDSA settings and the same DER encoder, and a second
 * implementation of any of that is a second thing to get subtly wrong. */

#include <stdint.h>

/* FURI_PACKED. This header is included from ctap2_crypto.h before anything
 * else pulls furi in, so it cannot rely on the caller having done so. */
#include <furi.h>
#include <mbedtls/ecp.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U2F_EC_KEY_SIZE    32
#define U2F_EC_BIGNUM_SIZE 32
#define U2F_EC_POINT_SIZE  65

/* Uncompressed SEC1 point: 0x04 followed by X and Y. */
typedef struct {
    uint8_t format;
    uint8_t xy[64];
} FURI_PACKED U2fPubKey;
_Static_assert(sizeof(U2fPubKey) == U2F_EC_POINT_SIZE, "U2fPubKey size mismatch");

/** RNG callback in mbedtls' shape, backed by furi_hal_random. */
int u2f_uecc_random_cb(void* context, uint8_t* dest, unsigned size);

/** ECDSA over a 32-byte hash. `signature` receives raw R‖S, 64 bytes. */
void u2f_ecc_sign(
    mbedtls_ecp_group* grp,
    const uint8_t* key,
    uint8_t* hash,
    uint8_t* signature);

void u2f_ecc_compute_public_key(
    mbedtls_ecp_group* grp,
    const uint8_t* private_key,
    U2fPubKey* public_key);

/** Wrap raw R‖S as a DER SEQUENCE of two INTEGERs. Returns the encoded length.
 * Used by both the U2F authenticate response and CTAP2 packed attestation. */
uint8_t u2f_der_encode_signature(uint8_t* der, uint8_t* sig);

#ifdef __cplusplus
}
#endif
