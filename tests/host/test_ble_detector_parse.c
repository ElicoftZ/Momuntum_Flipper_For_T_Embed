#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../applications/main/ble_detector/ble_detector_parse.h"

static void test_name(void) {
    const uint8_t ad[] = {6, 9, 'H', 'C', '-', '0', '5'};
    DetectorMatch match;
    assert(ble_detector_parse(ad, sizeof(ad), &match));
    assert(match.kinds & DETECTOR_BIT(DetectorSkimmer));
}

static void test_meta_and_fast_pair(void) {
    const uint8_t ad[] = {3, 3, 0x5f, 0xfd, 6, 0x16, 0x2c, 0xfe, 0, 0, 0x06};
    DetectorMatch match;
    assert(ble_detector_parse(ad, sizeof(ad), &match));
    assert(match.kinds & DETECTOR_BIT(DetectorMeta));
    assert(match.kinds & DETECTOR_BIT(DetectorPixelBuds));
}

static void test_flock_company(void) {
    const uint8_t ad[] = {3, 0xff, 0xc8, 0x09};
    DetectorMatch match;
    assert(ble_detector_parse(ad, sizeof(ad), &match));
    assert(match.kinds & DETECTOR_BIT(DetectorFlock));
}

static void test_malformed(void) {
    const uint8_t ad[] = {10, 9, 't'};
    DetectorMatch match;
    assert(!ble_detector_parse(ad, sizeof(ad), &match));
}

int main(void) {
    test_name();
    test_meta_and_fast_pair();
    test_flock_company();
    test_malformed();
    puts("BLE Detector parser tests passed");
    return 0;
}
