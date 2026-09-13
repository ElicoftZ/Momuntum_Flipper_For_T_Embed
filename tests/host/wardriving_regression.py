"""Compile actual scan/identity code with fake radio results (run in an MSVC shell)."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
wifi = (root / "components/wifi/wlan_hal.c").read_text()
ward = (root / "applications/main/wardriving/wardriving.c").read_text()
scan = wifi.split("case WCMD_SCAN: {", 1)[1].split("case WCMD_RUN_FN:", 1)[0]
identity = ward.split("        uint8_t mac[6];", 1)[1].split("        entry.rssi = disc->rssi;", 1)[0]
wifi_identity = ward.split("    const char* vendor = (ap->bssid[0]", 1)[1].split(
    "    entry.rssi = ap->rssi;", 1)[0]
update = ward.split('        if(strncmp(candidate->vendor, "Adv: ", 5)', 1)[1].split(
    "        furi_mutex_release(app->lock);", 1)[0]
tracker_fn = "static WardriveTracker wardrive_tracker_identify" + ward.split(
    "static WardriveTracker wardrive_tracker_identify", 1)[1].split("\n}\n", 1)[0] + "\n}\n"
centroid_fn = "static bool wardrive_position_estimate" + ward.split(
    "static bool wardrive_position_estimate", 1)[1].split("\n}\n", 1)[0] + "\n}\n"
payload_fn = "static uint8_t wardrive_build_emu_payload" + ward.split(
    "static uint8_t wardrive_build_emu_payload", 1)[1].split("\n}\n", 1)[0] + "\n}\n"
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef enum {
    WardriveTrackerNone = 0,
    WardriveTrackerAirTag,
    WardriveTrackerGoogle,
    WardriveTrackerSamsung,
} WardriveTracker;
#define ESP_OK 0
#define BLE_ADDR_PUBLIC 0
#define ESP_LOGE(...) ((void)0)
typedef int esp_err_t;
typedef struct { int marker; } wifi_ap_record_t;
static int start_error, count_error, records_error, allocation_error, cleared;
static uint16_t available;
static int esp_wifi_scan_start(void* config, bool blocking) { return start_error; }
static int esp_wifi_scan_get_ap_num(uint16_t* n) { *n = available; return count_error; }
static int esp_wifi_scan_get_ap_records(uint16_t* n, wifi_ap_record_t* p) {
    if(!records_error) p->marker = 123;
    return records_error;
}
static void esp_wifi_clear_ap_list(void) { ++cleared; }
static void* test_malloc(size_t n) { return allocation_error ? NULL : malloc(n); }
#define malloc test_malloc
static bool scan_test(wifi_ap_record_t** out, uint16_t* count) {
    *out = NULL; *count = 0;
    struct { struct { void* config; wifi_ap_record_t** out_records;
        uint16_t* out_count; uint16_t max_count; } scan; } cmd = {{NULL, out, count, 64}};
    bool ok = true;
    esp_err_t err = ESP_OK;
    switch(0) { case 0: {
''' + scan + r'''
    }
    return ok;
}
#undef malloc
static size_t strlcpy(char* out, const char* in, size_t size) {
    size_t n = strlen(in);
    if(size) { size_t k = n < size - 1 ? n : size - 1; memcpy(out, in, k); out[k] = 0; }
    return n;
}
static const char* wlan_oui_lookup(void* table, const uint8_t mac[6]) {
    return table && mac[0] == 0x28 && mac[1] == 0x6F && mac[2] == 0xB9 ? "Nokia" : NULL;
}
typedef struct { char vendor[32]; } Entry;
static Entry identify(uint8_t type, const uint8_t* data, size_t size, bool database) {
    struct { void* oui; } owner = {database ? (void*)1 : NULL}, *app = &owner;
    struct { struct { uint8_t type; uint8_t val[6]; } addr;
        const uint8_t* data; size_t length_data; } record = {{type,{1,2,3,0xB9,0x6F,0x28}},data,size}, *disc = &record;
    Entry entry = {0};
    uint8_t mac[6];
''' + identity + r'''
    return entry;
}
static void update_vendor(Entry* entry, const Entry* candidate) {
    if(strncmp(candidate->vendor, "Adv: ", 5)''' + update + r'''
}
static Entry identify_wifi(uint8_t first, bool database) {
    struct { void* oui; } owner = {database ? (void*)1 : NULL}, *app = &owner;
    struct { uint8_t bssid[6]; } record = {{first,0x6F,0xB9,1,2,3}}, *ap = &record;
    Entry entry = {0};
    const char* vendor = (ap->bssid[0]''' + wifi_identity + r'''
    return entry;
}
static uint8_t ble_spam_build_samsung_buds(uint8_t* buf, uint8_t r, uint8_t g, uint8_t b) {
    memset(buf, 0xAB, 31);
    buf[0] = 0x1B; buf[1] = 0xFF; buf[2] = 0x75; buf[3] = r; buf[4] = g; buf[5] = b;
    return 31;
}
/*TRACKER_FN*/
/*CENTROID_FN*/
/*PAYLOAD_FN*/
int main(void) {
    wifi_ap_record_t* p;
    uint16_t n;
    available = 3;
    assert(scan_test(&p, &n) && n == 3 && p->marker == 123); free(p);
    start_error = 1;
    assert(!scan_test(&p, &n) && p == NULL && n == 0); start_error = 0;
    count_error = 1; cleared = 0;
    assert(!scan_test(&p, &n) && p == NULL && n == 0 && cleared == 1); count_error = 0;
    allocation_error = 1; cleared = 0;
    assert(!scan_test(&p, &n) && p == NULL && n == 0 && cleared == 1); allocation_error = 0;
    records_error = 1; cleared = 0;
    assert(!scan_test(&p, &n) && p == NULL && n == 0 && cleared == 1); records_error = 0;
    available = 0;
    assert(scan_test(&p, &n) && p == NULL && n == 0);
    available = 100;
    assert(scan_test(&p, &n) && n == 64); free(p);
    uint8_t apple[] = {3, 0xFF, 0x4C, 0};
    uint8_t samsung[] = {3, 0xFF, 0x75, 0};
    Entry a = identify(1, apple, sizeof apple, true);
    assert(strcmp(a.vendor, "Adv: Apple") == 0);
    assert(strcmp(identify(1, samsung, sizeof samsung, false).vendor, "Adv: Samsung") == 0);
    assert(strcmp(identify(0, NULL, 0, true).vendor, "OUI: Nokia") == 0);
    assert(strcmp(identify(0, NULL, 0, false).vendor, "OUI DB missing") == 0);
    assert(strcmp(identify_wifi(0x28, true).vendor, "OUI: Nokia") == 0);
    assert(strcmp(identify_wifi(0x2A, true).vendor, "Local MAC / unknown") == 0);
    assert(strcmp(identify_wifi(0x28, false).vendor, "OUI DB missing") == 0);
    Entry unknown = identify(1, NULL, 0, true);
    assert(strcmp(unknown.vendor, "Random MAC / unknown") == 0);
    update_vendor(&a, &unknown);
    assert(strcmp(a.vendor, "Adv: Apple") == 0);
    update_vendor(&unknown, &a);
    assert(strcmp(unknown.vendor, "Adv: Apple") == 0);
    for(size_t i = 0; i < sizeof apple; ++i)
        assert(strcmp(identify(1, apple, i, true).vendor, "Random MAC / unknown") == 0);
    uint8_t airtag[] = {4, 0xFF, 0x4C, 0x00, 0x12};
    uint8_t apple_other[] = {3, 0xFF, 0x4C, 0x00};
    uint8_t samsung_mfg[] = {3, 0xFF, 0x75, 0x00};
    uint8_t google_mfg[] = {3, 0xFF, 0xE0, 0x00};
    uint8_t smarttag_uuid[] = {3, 0x03, 0x5A, 0xFD};
    uint8_t fastpair_uuid[] = {3, 0x03, 0x2C, 0xFE};
    assert(wardrive_tracker_identify(airtag, sizeof airtag) == WardriveTrackerAirTag);
    assert(wardrive_tracker_identify(apple_other, sizeof apple_other) == WardriveTrackerNone);
    assert(wardrive_tracker_identify(samsung_mfg, sizeof samsung_mfg) == WardriveTrackerSamsung);
    assert(wardrive_tracker_identify(google_mfg, sizeof google_mfg) == WardriveTrackerGoogle);
    assert(wardrive_tracker_identify(smarttag_uuid, sizeof smarttag_uuid) == WardriveTrackerSamsung);
    assert(wardrive_tracker_identify(fastpair_uuid, sizeof fastpair_uuid) == WardriveTrackerGoogle);
    assert(wardrive_tracker_identify(NULL, 0) == WardriveTrackerNone);
    assert(wardrive_tracker_identify(apple, 1) == WardriveTrackerNone);
    float lats[] = {10.0f, 20.0f};
    float lons[] = {30.0f, 40.0f};
    int8_t rssi_eq[] = {-40, -40};
    float la = 0, lo = 0;
    assert(wardrive_position_estimate(lats, lons, rssi_eq, 2, &la, &lo));
    assert(la > 14.99f && la < 15.01f && lo > 34.99f && lo < 35.01f);
    int8_t rssi_skew[] = {-40, -90};
    assert(wardrive_position_estimate(lats, lons, rssi_skew, 2, &la, &lo));
    assert(la > 11.0f && la < 12.0f && lo > 31.0f && lo < 32.0f);
    assert(!wardrive_position_estimate(lats, lons, rssi_eq, 0, &la, &lo));
    uint8_t payload[31];
    assert(wardrive_build_emu_payload(WardriveTrackerAirTag, payload) == 31);
    assert(payload[0] == 0x1E && payload[1] == 0xFF);
    assert(payload[2] == 0x4C && payload[3] == 0x00 && payload[4] == 0x12);
    assert(wardrive_build_emu_payload(WardriveTrackerSamsung, payload) == 31);
    assert(payload[0] == 0x1B && payload[1] == 0xFF && payload[2] == 0x75);
    assert(wardrive_build_emu_payload(WardriveTrackerGoogle, payload) == 0);
    puts("PASS: scan success/empty/cap/errors/cleanup; public/random MAC; manufacturer hints; truncated BLE data; hint retention; tracker brands; rssi centroid; emulation payloads");
}
'''
harness = harness.replace("/*TRACKER_FN*/", tracker_fn)
harness = harness.replace("/*CENTROID_FN*/", centroid_fn)
harness = harness.replace("/*PAYLOAD_FN*/", payload_fn)
build = root / "build_host"
build.mkdir(exist_ok=True)
source = build / "wardriving_regression.c"
source.write_text(harness)
subprocess.run(["cl", "/nologo", "/std:c17", str(source),
                "/Fe:" + str(build / "wardriving_regression.exe"),
                "/Fo:" + str(build / "wardriving_regression.obj")], check=True)
subprocess.run([str(build / "wardriving_regression.exe")], check=True)
