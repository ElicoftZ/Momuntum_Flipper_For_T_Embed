#include "../../applications/main/ble_spam/whisper_pair_ad.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* AD flags before FE2C service data: a three-byte model in display order. */
    const uint8_t pairing[] = {2, 1, 6, 6, 0x16, 0x2C, 0xFE, 0x30, 0x01, 0x8E};
    WhisperPairAdvertisement ad = whisper_pair_parse_ad(pairing, sizeof(pairing));
    assert(ad.present && ad.pairing_mode && ad.has_model && ad.model == 0x30018E);
    assert(!ad.account_filter);
    /* Every truncated prefix must be ignored rather than read out of bounds. */
    for(size_t i = 0; i < sizeof(pairing); ++i) {
        assert(!whisper_pair_parse_ad(pairing, i).present);
    }
    const uint8_t account[] = {7, 0x16, 0x2C, 0xFE, 0x20, 0xAA, 0xBB, 0xCC};
    ad = whisper_pair_parse_ad(account, sizeof(account));
    assert(ad.present && ad.account_filter && !ad.has_model && !ad.pairing_mode);
    const uint8_t extended[] = {7, 0x16, 0x2C, 0xFE, 0x01, 0x02, 0x03, 0x04};
    ad = whisper_pair_parse_ad(extended, sizeof(extended));
    assert(ad.present && ad.has_model && ad.model == 0x010203 && !ad.pairing_mode);
    const uint8_t empty[] = {3, 0x16, 0x2C, 0xFE};
    ad = whisper_pair_parse_ad(empty, sizeof(empty));
    assert(ad.present && !ad.has_model && !ad.pairing_mode);
    const uint8_t unrelated[] = {3, 0x16, 0x0F, 0x18};
    assert(!whisper_pair_parse_ad(unrelated, sizeof(unrelated)).present);
    const uint8_t invalid[] = {255, 0x16, 0x2C, 0xFE};
    assert(!whisper_pair_parse_ad(invalid, sizeof(invalid)).present);
    const uint8_t uuid_only[] = {3, 0x03, 0x2C, 0xFE};
    assert(!whisper_pair_parse_ad(uuid_only, sizeof(uuid_only)).present);
    assert(!whisper_pair_parse_ad(NULL, 0).present);
    const uint8_t address[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    const uint8_t salt[] = {0, 1, 2, 3, 4, 5, 6, 7};
    const uint8_t expected[] = {0, 0x11, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t request[16];
    whisper_pair_test_request(request, address, salt);
    assert(memcmp(request, expected, sizeof(expected)) == 0);
    puts("WhisperPair: advertisement bounds, upstream modes and request vector passed");
    return 0;
}
