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

static void test_tile_service(void) {
    const uint8_t ad[] = {3, 3, 0xED, 0xFE};
    DetectorMatch match;
    assert(ble_detector_parse(ad, sizeof(ad), &match));
    assert(match.kinds & DETECTOR_BIT(DetectorTile));
}

static void test_beats_proximity_pairing(void) {
    /* Apple ProximityPair, device id 0x1120 = Beats Studio Buds. */
    uint8_t ad[32] = {0x1F, 0xFF, 0x4C, 0x00, 0x07, 0x19, 0x07, 0x11, 0x20};
    DetectorMatch match;
    assert(ble_detector_parse(ad, sizeof(ad), &match));
    assert(match.kinds & DETECTOR_BIT(DetectorBeats));
    assert(!(match.kinds & DETECTOR_BIT(DetectorAirPods)));
}

static void test_apple_host_nearby(void) {
    const uint8_t ad[] = {10, 0xFF, 0x4C, 0x00, 0x10, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05};
    DetectorMatch match;
    assert(ble_detector_parse(ad, sizeof(ad), &match));
    assert(match.kinds & DETECTOR_BIT(DetectorAppleHost));
}

static void test_xiaomi_company(void) {
    const uint8_t ad[] = {3, 0xFF, 0x8F, 0x03};
    DetectorMatch match;
    assert(ble_detector_parse(ad, sizeof(ad), &match));
    assert(match.kinds & DETECTOR_BIT(DetectorXiaomi));
}

static void test_galaxy_buds_fast_pair(void) {
    /* Fast Pair service data, model 0x0082DA = Galaxy Buds2 Pro. */
    const uint8_t ad[] = {6, 0x16, 0x2C, 0xFE, 0x00, 0x82, 0xDA};
    DetectorMatch match;
    assert(ble_detector_parse(ad, sizeof(ad), &match));
    assert(match.kinds & DETECTOR_BIT(DetectorGalaxyBuds));
}

static void test_garmin_company(void) {
    const uint8_t ad[] = {3, 0xFF, 0x87, 0x00};
    DetectorMatch match;
    assert(ble_detector_parse(ad, sizeof(ad), &match));
    assert(match.kinds & DETECTOR_BIT(DetectorGarmin));
}

static void test_fitbit_name(void) {
    const uint8_t ad[] = {7, 9, 'F', 'i', 't', 'b', 'i', 't'};
    DetectorMatch match;
    assert(ble_detector_parse(ad, sizeof(ad), &match));
    assert(match.kinds & DETECTOR_BIT(DetectorFitbit));
}

static void test_gopro_company(void) {
    const uint8_t ad[] = {3, 0xFF, 0xF2, 0x02};
    DetectorMatch match;
    assert(ble_detector_parse(ad, sizeof(ad), &match));
    assert(match.kinds & DETECTOR_BIT(DetectorGoPro));
}

int main(void) {
    test_name();
    test_meta_and_fast_pair();
    test_flock_company();
    test_malformed();
    test_tile_service();
    test_beats_proximity_pairing();
    test_apple_host_nearby();
    test_xiaomi_company();
    test_galaxy_buds_fast_pair();
    test_garmin_company();
    test_fitbit_name();
    test_gopro_company();
    puts("BLE Detector parser tests passed");
    return 0;
}
