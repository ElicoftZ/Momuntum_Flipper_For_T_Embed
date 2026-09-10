#include "ble_detector_parse.h"
#include <string.h>

const char* const detector_labels[DetectorKindCount] = {
    "All Scan", "Flipper Zero", "Flock camera", "Axon body camera",
    "Credit card skimmer", "Meta Quest / glasses", "Samsung SmartTag",
    "Apple AirTag", "AirPods", "Microsoft", "Google Pixel Buds",
};

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void signature(DetectorMatch* m, DetectorKind kind) {
    m->kinds |= DETECTOR_BIT(kind);
    m->signatures |= DETECTOR_BIT(kind);
}

static void service(DetectorMatch* m, uint16_t uuid) {
    if(uuid >= 0x3080 && uuid <= 0x3083) signature(m, DetectorFlipper);
    if(uuid == 0xFD5F) signature(m, DetectorMeta);
    if(uuid == 0xFD5A) signature(m, DetectorSmartTag);
}

static bool contains(const uint8_t* p, size_t size, const char* text) {
    size_t n = strlen(text);
    for(size_t i = 0; i + n <= size; ++i) {
        if(!memcmp(p + i, text, n)) return true;
    }
    return false;
}

static void name_hints(DetectorMatch* m) {
    char name[sizeof(m->name)];
    for(size_t i = 0; i < sizeof(name); ++i) {
        char c = m->name[i];
        name[i] = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    }
    if(!strncmp(name, "flipper", 7)) m->kinds |= DETECTOR_BIT(DetectorFlipper);
    bool penguin = !strncmp(name, "penguin-", 8) && strlen(name) == 18;
    for(size_t i = 8; penguin && i < 18; ++i) penguin = name[i] >= '0' && name[i] <= '9';
    if(penguin || !strncmp(name, "flock", 5) || !strcmp(name, "fs ext battery"))
        m->kinds |= DETECTOR_BIT(DetectorFlock);
    if(!strncmp(name, "axon body", 9)) m->kinds |= DETECTOR_BIT(DetectorAxon);
    // Generic serial modules are only a possible skimmer, never proof.
    if(!strcmp(name, "hc-03") || !strcmp(name, "hc-05") || !strcmp(name, "hc-06"))
        m->kinds |= DETECTOR_BIT(DetectorSkimmer);
    if(!strncmp(name, "meta quest", 10) || !strncmp(name, "oculus", 6) ||
       !strncmp(name, "ray-ban", 7)) m->kinds |= DETECTOR_BIT(DetectorMeta);
    if(strstr(name, "smarttag")) m->kinds |= DETECTOR_BIT(DetectorSmartTag);
    if(strstr(name, "airtag")) m->kinds |= DETECTOR_BIT(DetectorAirTag);
    if(strstr(name, "airpods")) m->kinds |= DETECTOR_BIT(DetectorAirPods);
    if(!strncmp(name, "microsoft", 9)) m->kinds |= DETECTOR_BIT(DetectorMicrosoft);
    if(strstr(name, "pixel buds")) m->kinds |= DETECTOR_BIT(DetectorPixelBuds);
}

bool ble_detector_parse(const uint8_t* data, size_t length, DetectorMatch* match) {
    if(!match) return false;
    memset(match, 0, sizeof(*match));
    if(!data) return false;
    DetectorMatch m = {0};
    for(size_t pos = 0; pos < length;) {
        size_t size = data[pos++];
        if(!size) break;
        if(size > length - pos) return false;
        uint8_t type = data[pos];
        const uint8_t* p = data + pos + 1;
        size_t n = size - 1;
        if(type == 0x08 || type == 0x09) {
            size_t copy = n < sizeof(m.name) - 1 ? n : sizeof(m.name) - 1;
            for(size_t i = 0; i < copy; ++i) m.name[i] = p[i] >= 32 && p[i] < 127 ? p[i] : '.';
            m.name[copy] = 0;
        } else if(type == 0x02 || type == 0x03) {
            if(n % 2) return false;
            for(size_t i = 0; i < n; i += 2) service(&m, le16(p + i));
        } else if(type == 0x06 || type == 0x07) {
            static const uint8_t base[12] = {0xfb,0x34,0x9b,0x5f,0x80,0,0,0x80,0,0x10,0,0};
            if(n % 16) return false;
            for(size_t i = 0; i < n; i += 16) {
                if(!memcmp(p + i, base, 12) && !p[i+14] && !p[i+15]) service(&m, le16(p+i+12));
            }
        } else if(type == 0x16 && n >= 2) {
            uint16_t uuid = le16(p);
            service(&m, uuid);
            // Model IDs from the cited Fast Pair research dataset; no generic Google match.
            if(uuid == 0xFE2C && n == 5) {
                uint32_t model = ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4];
                if(model == 6 || model == 12934265 || model == 10148625) signature(&m, DetectorPixelBuds);
            }
            if(contains(p + 2, n - 2, "BWCDEVICE")) signature(&m, DetectorAxon);
        } else if(type == 0xFF && n >= 2) {
            uint16_t company = le16(p);
            if(company == 0x0006) signature(&m, DetectorMicrosoft);
            if(company == 0x09C8) signature(&m, DetectorFlock);
            if(company == 0x004C && n >= 4 && p[3] <= n - 4) {
                // Find My is shared by third-party accessories: UI labels this as a candidate.
                if(p[2] == 0x12 && p[3] == 0x19) signature(&m, DetectorAirTag);
                if(p[2] == 0x07 && p[3] == 0x19) {
                    uint16_t model = ((uint16_t)p[5] << 8) | p[6];
                    switch(model) {
                    case 0x0220: case 0x0F20: case 0x1320: case 0x0E20: case 0x1420:
                    case 0x2420: case 0x2820: case 0x2920: case 0x0A20: case 0x2B20:
                        signature(&m, DetectorAirPods); break;
                    default: break;
                    }
                }
            }
        }
        pos += size;
    }
    name_hints(&m);
    *match = m;
    return m.kinds != 0;
}
