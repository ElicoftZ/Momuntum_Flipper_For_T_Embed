#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t mac[6];
    uint8_t ssid_length;
    uint8_t ssid[32];
} WlanProbe;

/* Length excludes the four-byte FCS. Accept only complete probe requests. */
static inline bool wlan_probe_parse(const uint8_t* frame, size_t length, WlanProbe* out) {
    if(!frame || !out || length < 26 || frame[0] != 0x40 || (frame[1] & 0x43))
        return false;
    WlanProbe probe = {0};
    memcpy(probe.mac, frame + 10, 6);
    bool found = false;
    for(size_t pos = 24; pos < length;) {
        if(length - pos < 2) return false;
        uint8_t id = frame[pos++];
        uint8_t size = frame[pos++];
        if(size > length - pos) return false;
        if(id == 0) {
            if(found || size > sizeof(probe.ssid)) return false;
            found = true;
            probe.ssid_length = size;
            memcpy(probe.ssid, frame + pos, size);
        }
        pos += size;
    }
    if(found) *out = probe;
    return found;
}
