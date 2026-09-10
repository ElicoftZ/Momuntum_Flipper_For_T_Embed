#include <assert.h>
#include <stdio.h>
#include "../../applications/main/wlan_app/wlan_probe.h"

int main(void) {
    uint8_t frame[80] = {0x40};
    memcpy(frame + 10, "ABCDEF", 6);
    WlanProbe probe;
    assert(wlan_probe_parse(frame, 26, &probe));
    assert(probe.ssid_length == 0);
    assert(memcmp(probe.mac, "ABCDEF", 6) == 0);
    frame[25] = 4;
    memcpy(frame + 26, "test", 4);
    assert(wlan_probe_parse(frame, 30, &probe));
    assert(probe.ssid_length == 4 && !memcmp(probe.ssid, "test", 4));
    for(size_t i = 0; i < 30; ++i) assert(!wlan_probe_parse(frame, i, &probe));
    frame[0] = 0x80; /* Beacon */
    assert(!wlan_probe_parse(frame, 30, &probe));
    frame[0] = 0x50; /* Probe response */
    assert(!wlan_probe_parse(frame, 30, &probe));
    frame[0] = 0x40;
    frame[1] = 0x40; /* Protected */
    assert(!wlan_probe_parse(frame, 30, &probe));
    frame[1] = 0x08; /* Retry is legal */
    assert(wlan_probe_parse(frame, 30, &probe));
    frame[25] = 32;
    memset(frame + 26, 0xFF, 32);
    assert(wlan_probe_parse(frame, 58, &probe));
    assert(probe.ssid_length == 32 && probe.ssid[31] == 0xFF);
    frame[25] = 33;
    assert(!wlan_probe_parse(frame, 59, &probe));
    frame[25] = 32;
    assert(!wlan_probe_parse(frame, 59, &probe)); /* Partial IE */
    assert(!wlan_probe_parse(frame, 60, &probe)); /* Duplicate SSID */
    frame[58] = 1;
    frame[59] = 2;
    assert(!wlan_probe_parse(frame, 61, &probe));
    assert(wlan_probe_parse(frame, 62, &probe));
    frame[24] = 1;
    assert(!wlan_probe_parse(frame, 62, &probe)); /* No SSID */
    assert(!wlan_probe_parse(NULL, 62, &probe));
    puts("Probe parser tests passed");
    return 0;
}
