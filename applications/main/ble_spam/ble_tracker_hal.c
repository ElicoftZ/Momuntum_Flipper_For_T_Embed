#include "ble_tracker_hal.h"

#include <esp_log.h>
#include <furi.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <nimble_glue.h>
#include <string.h>

#define TAG "BleTracker"

static TrackerDevice s_devices[TRACKER_MAX_DEVICES];
static uint16_t s_device_count = 0;
static volatile bool s_scanning = false;

// ---------------------------------------------------------------------------
// Apple Proximity Pairing model table — ported from Poseidon ble_db.cpp:154
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t key;
    const char* name;
} apple_pp_model_t;

static const apple_pp_model_t APPLE_PP[] = {
    {0x01, "AirPods 1"},
    {0x02, "AirPods Pro"},
    {0x03, "AirPods Max"},
    {0x04, "AppleTV Setup"},
    {0x05, "Beats X"},
    {0x06, "Beats Solo 3"},
    {0x07, "Beats Studio 3"},
    {0x09, "Beats Studio Pro"},
    {0x0A, "Beats Fit Pro"},
    {0x0B, "Beats Flex"},
    {0x0C, "Beats Solo Pro"},
    {0x0D, "Beats Studio Buds"},
    {0x0E, "Beats Studio Buds+"},
    {0x0F, "AirPods 2"},
    {0x10, "AirPods 3"},
    {0x11, "AirPods 4"},
    {0x13, "AirPods Pro 2"},
    {0x14, "AirPods Pro 2 USB-C"},
    {0x19, "PowerBeats Pro"},
    {0x1A, "Beats Fit Pro 2"},
    {0x00, NULL},
};

static const char* apple_pp_lookup(uint8_t model) {
    for(const apple_pp_model_t* p = APPLE_PP; p->name; ++p) {
        if(p->key == model) return p->name;
    }
    return "Apple pairing";
}

// ---------------------------------------------------------------------------
// AD parsing helpers
// ---------------------------------------------------------------------------

// Walks one BLE advertisement TLV buffer and looks for any of:
//   - manufacturer data (type 0xFF) for tracker matchers
//   - 16-bit Service UUID lists (types 0x02/0x03) for Tile (0xFEED, 0xFD84)
// Outputs classification + optional model name into kind_out, name_out.
// Returns true if a tracker kind was identified.
static bool classify_buffer(
    const uint8_t* buf,
    uint8_t len,
    TrackerKind* kind_out,
    char* name_out,
    uint8_t name_out_sz) {
    uint8_t pos = 0;
    while(pos + 1 < len) {
        uint8_t fld_len = buf[pos];
        if(fld_len == 0) break;
        if(pos + fld_len >= len) break;
        uint8_t type = buf[pos + 1];
        const uint8_t* val = &buf[pos + 2];
        uint8_t val_len = fld_len - 1;

        if(type == 0xFF && val_len >= 2) {
            uint16_t cid = val[0] | (val[1] << 8);
            // Apple
            if(cid == 0x004C && val_len >= 3) {
                uint8_t sub = val[2];
                if(sub == 0x12) {
                    *kind_out = TrackerKindAirTag;
                    name_out[0] = '\0';
                    return true;
                }
                if(sub == 0x10) {
                    *kind_out = TrackerKindAppleNearby;
                    name_out[0] = '\0';
                    return true;
                }
                if(sub == 0x07 && val_len >= 4) {
                    *kind_out = TrackerKindApplePP;
                    strncpy(name_out, apple_pp_lookup(val[3]), name_out_sz - 1);
                    name_out[name_out_sz - 1] = '\0';
                    return true;
                }
            }
            // Samsung
            if(cid == 0x0075) {
                *kind_out = TrackerKindSmartTag;
                name_out[0] = '\0';
                return true;
            }
        } else if((type == 0x02 || type == 0x03) && val_len >= 2) {
            // Incomplete / Complete list of 16-bit Service Class UUIDs
            for(uint8_t i = 0; i + 1 < val_len; i += 2) {
                uint16_t u = val[i] | (val[i + 1] << 8);
                if(u == 0xFEED || u == 0xFD84) {
                    *kind_out = TrackerKindTile;
                    name_out[0] = '\0';
                    return true;
                }
            }
        }

        pos += fld_len + 1;
    }
    return false;
}

static bool classify_advert(
    const uint8_t* buf,
    uint8_t adv_len,
    uint8_t scan_rsp_len,
    TrackerKind* kind_out,
    char* name_out,
    uint8_t name_out_sz) {
    if(classify_buffer(buf, adv_len, kind_out, name_out, name_out_sz)) return true;
    if(scan_rsp_len > 0 &&
       classify_buffer(buf + adv_len, scan_rsp_len, kind_out, name_out, name_out_sz))
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// GAP callback
// ---------------------------------------------------------------------------

static int tracker_gap_event_handler(struct ble_gap_event* event, void* context) {
    (void)context;
    switch(event->type) {
    case BLE_GAP_EVENT_DISC: {
            const struct ble_gap_disc_desc* result = &event->disc;
            TrackerKind kind = TrackerKindUnknown;
            char name[24] = "";
            bool match = classify_advert(
                result->data,
                result->length_data,
                0,
                &kind,
                name,
                sizeof(name));
            if(!match) break;

            uint32_t now = furi_get_tick();

            // Dedup by MAC
            uint8_t display_addr[6];
            for(size_t i = 0; i < sizeof(display_addr); i++) {
                display_addr[i] = result->addr.val[sizeof(display_addr) - 1 - i];
            }
            int found = -1;
            for(int i = 0; i < s_device_count; i++) {
                if(memcmp(s_devices[i].addr, display_addr, 6) == 0) {
                    found = i;
                    break;
                }
            }
            if(found >= 0) {
                TrackerDevice* d = &s_devices[found];
                d->rssi = result->rssi;
                if(result->rssi > d->best_rssi) d->best_rssi = result->rssi;
                d->last_seen_ms = now;
                // Upgrade name if PP gave us a model after a generic match
                if(d->name[0] == '\0' && name[0] != '\0') {
                    strncpy(d->name, name, sizeof(d->name) - 1);
                    d->name[sizeof(d->name) - 1] = '\0';
                }
                d->kind = kind;
            } else if(s_device_count < TRACKER_MAX_DEVICES) {
                TrackerDevice* d = &s_devices[s_device_count];
                memcpy(d->addr, display_addr, 6);
                d->addr_type = result->addr.type;
                d->kind = kind;
                d->rssi = result->rssi;
                d->best_rssi = result->rssi;
                d->last_seen_ms = now;
                strncpy(d->name, name, sizeof(d->name) - 1);
                d->name[sizeof(d->name) - 1] = '\0';
                s_device_count++;
            }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        break;

    default:
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ble_tracker_hal_start_scan(void) {
    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));

    struct ble_gap_disc_params scan_params = {
        .passive = 1,
        .itvl = 0x50,
        .window = 0x30,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(
        nimble_glue_own_address_type(),
        BLE_HS_FOREVER,
        &scan_params,
        tracker_gap_event_handler,
        NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed, rc=%d", rc);
        return false;
    }
    s_scanning = true;
    return true;
}

void ble_tracker_hal_stop_scan(void) {
    if(s_scanning) {
        ble_gap_disc_cancel();
        for(int i = 0; i < 20 && s_scanning; i++) {
            furi_delay_ms(5);
        }
    }
}

TrackerDevice* ble_tracker_hal_get_devices(uint16_t* count) {
    *count = s_device_count;
    return s_devices;
}

const char* ble_tracker_kind_label(TrackerKind k) {
    switch(k) {
    case TrackerKindAirTag: return "AirTag";
    case TrackerKindSmartTag: return "SmartTag";
    case TrackerKindTile: return "Tile";
    case TrackerKindApplePP: return "Apple";
    case TrackerKindAppleNearby: return "iPhone/Watch";
    default: return "?";
    }
}
