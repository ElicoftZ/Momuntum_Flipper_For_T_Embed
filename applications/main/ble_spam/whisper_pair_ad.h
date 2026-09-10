#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Adapted from zalexdev/wpair-app Scanner.kt, commit 7f5930ce3be645d5bc2e27df4c6e09f8fca49117.
 * Apache-2.0; see whisper_pair/NOTICE and LICENSE. Modified: bounded C AD parser.
 * Upstream pairing/account-data interpretations are heuristics, not proof. */
typedef struct {
    bool present;
    bool has_model;
    bool pairing_mode;
    bool account_filter;
    uint32_t model;
} WhisperPairAdvertisement;

static inline WhisperPairAdvertisement whisper_pair_parse_ad(const uint8_t* data, size_t size) {
    WhisperPairAdvertisement result = {0};
    for(size_t pos = 0; pos < size;) {
        size_t len = data[pos];
        if(len == 0 || len >= size - pos) break;
        const uint8_t type = data[pos + 1];
        if(type == 0x16 && len >= 3 && data[pos + 2] == 0x2C && data[pos + 3] == 0xFE) {
            result.present = true;
            const size_t payload_size = len - 3;
            const uint8_t first = payload_size ? data[pos + 4] : 0;
            if(payload_size == 3 && !(first & 0x80)) {
                result.pairing_mode = true;
                result.has_model = true;
            } else if(payload_size && (first & 0x60)) {
                result.account_filter = true;
            } else if(payload_size > 3 && !(first & 0x80)) {
                result.has_model = true;
            }
            if(result.has_model) {
                result.model = ((uint32_t)data[pos + 4] << 16) |
                               ((uint32_t)data[pos + 5] << 8) | data[pos + 6];
            }
            return result;
        }
        pos += len + 1;
    }
    return result;
}

/* Port of VulnerabilityTester.buildTestRequest. Salt is supplied by ESP RNG. */
static inline void whisper_pair_test_request(
    uint8_t request[16], const uint8_t address[6], const uint8_t salt[8]) {
    request[0] = 0x00;
    request[1] = 0x11;
    for(size_t i = 0; i < 6; ++i) request[2 + i] = address[i];
    for(size_t i = 0; i < 8; ++i) request[8 + i] = salt[i];
}
