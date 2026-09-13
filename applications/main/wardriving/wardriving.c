#include <furi.h>
#include <furi_hal.h>

#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <input/input.h>
#include <storage/storage.h>

#include <wifi/wlan_hal.h>
#include <btshim.h>
#include "../wlan_app/wlan_oui.h"
#include "../ble_spam/ble_spam_payloads.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <nimble_glue.h>

#include <lib/subghz/devices/cc1101_configs.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "Wardriving"

#define WARD_VIEW_MENU 0U
#define WARD_VIEW_SCAN 1U
#define WARD_VIEW_SETTINGS 2U

#define WARD_EVENT_SCROLL_UP   100U
#define WARD_EVENT_SCROLL_DOWN 101U
#define WARD_EVENT_SETTINGS    200U

#define WARD_MAX_ENTRIES       512U
#define WARD_VISIBLE_ENTRIES   2U
#define WARD_WORKER_STACK      (10U * 1024U)
#define WARD_LOG_SYNC_ROWS     8U
#define WARD_SUB_THRESHOLD_DBM (-85.0f)
#define WARD_SUB_MERGE_HZ      500000U

#define WARD_LOG_DIR EXT_PATH("wardriving")
#define WARD_FINDMY_CONFIG EXT_PATH("apps_data/wardriving/findmy.txt")
#define WARD_APDB_PATH EXT_PATH("apps_data/wardriving/apdb.bin")
#define WARD_APDB_MAGIC "WRAPDB1\0"
#define WARD_APDB_HEADER 16U
#define WARD_APDB_RECORD 14U

typedef enum {
    WardriveModeNone = 0,
    WardriveModeSubGhz = 1,
    WardriveModeBle = 2,
    WardriveModeWifi = 3,
    WardriveModeEmulate = 4,
} WardriveMode;

/* Tracker brands the FindMy setting can tag during a BLE wardrive. */
typedef enum {
    WardriveTrackerNone = 0,
    WardriveTrackerAirTag,
    WardriveTrackerGoogle,
    WardriveTrackerSamsung,
} WardriveTracker;

typedef struct {
    char name[33];
    char id[24];
    char details[20];
    char vendor[32];
    char brand[12];
    int16_t rssi;
    int16_t best_rssi;
    uint16_t channel;
    uint32_t frequency;
    uint32_t first_seen;
    uint32_t last_seen;
} WardriveEntry;

typedef struct {
    WardriveMode mode;
    uint16_t found;
    uint32_t dropped;
    bool running;
    bool have_location;
    float latitude;
    float longitude;
    char status[32];
    WardriveEntry rows[WARD_VISIBLE_ENTRIES];
    uint8_t row_count;
} WardriveViewModel;

typedef struct WardrivingApp WardrivingApp;

struct WardrivingApp {
    Gui* gui;
    Storage* storage;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    View* scan_view;
    VariableItemList* settings;
    FuriMutex* lock;
    FuriThread* worker;

    File* log_file;
    File* findmy_file;
    File* apdb_file;
    File* location_file;
    WardriveEntry* entries;
    WlanOuiTable* oui;
    WardriveMode mode;
    uint16_t count;
    uint16_t logged_count;
    uint16_t scroll;
    uint32_t dropped;
    uint32_t log_rows_since_sync;
    uint32_t findmy_rows_since_sync;
    uint32_t apdb_count;
    bool have_location;
    float last_lat;
    float last_lon;
    bool follow_latest;
    bool scan_view_active;
    bool settings_view_active;
    bool log_failed;
    bool save_map;
    WardriveTracker emu_tag_type;
    char status[32];
    char log_path[128];

    volatile bool stop_requested;
    volatile bool worker_active;
    volatile bool ble_scan_started;
    volatile bool ble_scan_failed;
    bool bt_was_enabled;
    bool menu_view_added;
    bool scan_view_added;
    bool settings_view_added;
};

static WardrivingApp* wardrive_ble_owner = NULL;

static uint8_t wardrive_visible_entries(WardriveMode mode) {
    return mode == WardriveModeSubGhz ? WARD_VISIBLE_ENTRIES : 1U;
}

static const char* wardrive_mode_label(WardriveMode mode) {
    switch(mode) {
    case WardriveModeSubGhz:
        return "Sub-GHz";
    case WardriveModeBle:
        return "BLE";
    case WardriveModeWifi:
        return "WiFi";
    case WardriveModeEmulate:
        return "Emulate";
    default:
        return "Wardrive";
    }
}

static const char* wardrive_mode_filename(WardriveMode mode) {
    switch(mode) {
    case WardriveModeSubGhz:
        return "subghz";
    case WardriveModeBle:
        return "ble";
    case WardriveModeWifi:
        return "wifi";
    case WardriveModeEmulate:
        return "emulate";
    default:
        return "scan";
    }
}

static const char* wardrive_tracker_label(WardriveTracker tracker) {
    switch(tracker) {
    case WardriveTrackerAirTag:
        return "AirTag";
    case WardriveTrackerGoogle:
        return "Google";
    case WardriveTrackerSamsung:
        return "Samsung";
    default:
        return "";
    }
}

/* Tags a raw advertisement as an AirTag/Google/Samsung tracker. Apple FindMy
 * uses mfg 0x004C + type 0x12, Samsung SmartTag mfg 0x0075 / service 0xFD5A,
 * and Google covers both the Find My Device company id 0x00E0 and Fast Pair
 * service 0xFE2C. The last matching field wins. Detection is always on: BLE
 * wardriving logs every device, and tracker hits are tagged as they arrive. */
static WardriveTracker wardrive_tracker_identify(const uint8_t* data, uint8_t data_len) {
    WardriveTracker found = WardriveTrackerNone;
    if(!data) return found;
    for(uint8_t pos = 0; pos + 1 < data_len;) {
        const uint8_t field_len = data[pos];
        if(field_len == 0 || pos + field_len >= data_len) break;
        const uint8_t type = data[pos + 1];
        const uint8_t* p = &data[pos + 2];
        const uint8_t n = field_len - 1;
        if(type == 0xFF && n >= 2) {
            const uint16_t company = (uint16_t)(p[0] | (p[1] << 8));
            if(company == 0x004C && n >= 3 && p[2] == 0x12) found = WardriveTrackerAirTag;
            else if(company == 0x0075) found = WardriveTrackerSamsung;
            else if(company == 0x00E0) found = WardriveTrackerGoogle;
        } else if((type == 0x02 || type == 0x03) && n >= 2) {
            for(uint8_t i = 0; i + 1 < n; i += 2) {
                const uint16_t uuid = (uint16_t)(p[i] | (p[i + 1] << 8));
                if(uuid == 0xFD5A) found = WardriveTrackerSamsung;
                else if(uuid == 0xFE2C) found = WardriveTrackerGoogle;
            }
        }
        pos += field_len + 1;
    }
    return found;
}

static void wardrive_set_status(WardrivingApp* app, const char* status) {
    if(!app || !app->lock) return;
    if(furi_mutex_acquire(app->lock, FuriWaitForever) != FuriStatusOk) return;
    strlcpy(app->status, status ? status : "", sizeof(app->status));
    furi_mutex_release(app->lock);
}

static void wardrive_findmy_load(WardrivingApp* app) {
    app->save_map = false;
    app->emu_tag_type = WardriveTrackerAirTag;
    if(!app->storage) return;

    File* file = storage_file_alloc(app->storage);
    if(!file) return;
    if(storage_file_open(file, WARD_FINDMY_CONFIG, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[64];
        const size_t read = storage_file_read(file, buf, sizeof(buf) - 1);
        buf[read] = '\0';
        const char* p;
        if((p = strstr(buf, "save="))) app->save_map = p[5] == '1';
        if((p = strstr(buf, "type="))) {
            const int type = p[5] - '0';
            if(type == WardriveTrackerAirTag || type == WardriveTrackerGoogle ||
               type == WardriveTrackerSamsung)
                app->emu_tag_type = (WardriveTracker)type;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
}

static void wardrive_findmy_save(WardrivingApp* app) {
    if(!app || !app->storage) return;
    storage_simply_mkdir(app->storage, EXT_PATH("apps_data/wardriving"));
    File* file = storage_file_alloc(app->storage);
    if(!file) return;
    if(storage_file_open(file, WARD_FINDMY_CONFIG, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char buf[48];
        const int len =
            snprintf(buf, sizeof(buf), "save=%d\ntype=%d\n", app->save_map, app->emu_tag_type);
        if(len > 0) storage_file_write(file, buf, (size_t)len);
        storage_file_close(file);
    }
    storage_file_free(file);
}

static void wardrive_sanitize_text(char* text) {
    if(!text) return;
    for(char* p = text; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if(!isprint(c) || c == '\r' || c == '\n') *p = ' ';
    }
}

static bool wardrive_entry_matches(
    WardriveMode mode,
    const WardriveEntry* existing,
    const WardriveEntry* candidate) {
    if(mode == WardriveModeSubGhz) {
        const uint32_t a = existing->frequency;
        const uint32_t b = candidate->frequency;
        return a > b ? (a - b <= WARD_SUB_MERGE_HZ) : (b - a <= WARD_SUB_MERGE_HZ);
    }
    return strcmp(existing->id, candidate->id) == 0;
}

static bool wardrive_add_or_update(WardrivingApp* app, const WardriveEntry* candidate) {
    if(!app || !candidate || !app->entries || !app->lock) return false;
    if(furi_mutex_acquire(app->lock, 0) != FuriStatusOk) return false;

    for(uint16_t i = 0; i < app->count; ++i) {
        WardriveEntry* entry = &app->entries[i];
        if(!wardrive_entry_matches(app->mode, entry, candidate)) continue;

        entry->rssi = candidate->rssi;
        entry->last_seen = candidate->last_seen;
        if(candidate->rssi > entry->best_rssi) {
            entry->best_rssi = candidate->rssi;
            if(app->mode == WardriveModeSubGhz) {
                entry->frequency = candidate->frequency;
                strlcpy(entry->id, candidate->id, sizeof(entry->id));
            }
        }
        if(entry->name[0] == '\0' && candidate->name[0] != '\0') {
            strlcpy(entry->name, candidate->name, sizeof(entry->name));
        }
        if(entry->brand[0] == '\0' && candidate->brand[0] != '\0') {
            strlcpy(entry->brand, candidate->brand, sizeof(entry->brand));
        }
        if(strncmp(candidate->vendor, "Adv: ", 5) == 0 ||
           (strncmp(entry->vendor, "Adv: ", 5) != 0 &&
            strncmp(candidate->vendor, "OUI: ", 5) == 0)) {
            strlcpy(entry->vendor, candidate->vendor, sizeof(entry->vendor));
        }
        furi_mutex_release(app->lock);
        return false;
    }

    if(app->count >= WARD_MAX_ENTRIES) {
        app->dropped++;
        furi_mutex_release(app->lock);
        return false;
    }

    app->entries[app->count] = *candidate;
    app->count++;
    if(app->follow_latest) {
        app->scroll = app->count > wardrive_visible_entries(app->mode) ?
                    app->count - wardrive_visible_entries(app->mode) : 0;
    }
    furi_mutex_release(app->lock);
    return true;
}

static bool wardrive_file_write(File* file, const void* data, size_t size) {
    return file && data && storage_file_write(file, data, size) == size;
}

static bool wardrive_csv_field(File* file, const char* text) {
    if(!wardrive_file_write(file, "\"", 1)) return false;
    if(text) {
        const char* run = text;
        const char* p = text;
        while(*p) {
            if(*p == '"' || *p == '\r' || *p == '\n') {
                if(p > run && !wardrive_file_write(file, run, (size_t)(p - run))) return false;
                if(*p == '"') {
                    if(!wardrive_file_write(file, "\"\"", 2)) return false;
                } else if(!wardrive_file_write(file, " ", 1)) {
                    return false;
                }
                run = p + 1;
            }
            ++p;
        }
        if(p > run && !wardrive_file_write(file, run, (size_t)(p - run))) return false;
    }
    return wardrive_file_write(file, "\"", 1);
}

/* Opened lazily on the first saved tracker hit so a plain wardrive never
 * creates the file. Runs on the worker thread, next to the main log writes. */
static bool wardrive_findmy_open(WardrivingApp* app) {
    if(app->findmy_file) return true;
    if(!app->storage || storage_sd_status(app->storage) != FSE_OK) return false;
    if(!storage_simply_mkdir(app->storage, WARD_LOG_DIR)) return false;

    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    char path[128];
    snprintf(
        path,
        sizeof(path),
        WARD_LOG_DIR "/findmy_%04u%02u%02u_%02u%02u%02u.csv",
        now.year,
        now.month,
        now.day,
        now.hour,
        now.minute,
        now.second);

    app->findmy_file = storage_file_alloc(app->storage);
    if(!app->findmy_file) return false;
    if(!storage_file_open(app->findmy_file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(app->findmy_file);
        app->findmy_file = NULL;
        return false;
    }
    static const char header[] = "timestamp,brand,name,id,rssi,best_rssi,channel\n";
    if(!wardrive_file_write(app->findmy_file, header, sizeof(header) - 1) ||
       !storage_file_sync(app->findmy_file)) {
        storage_file_close(app->findmy_file);
        storage_file_free(app->findmy_file);
        app->findmy_file = NULL;
        return false;
    }
    FURI_LOG_I(TAG, "FindMy log %s", path);
    return true;
}

static void wardrive_findmy_close(WardrivingApp* app) {
    if(!app || !app->findmy_file) return;
    storage_file_sync(app->findmy_file);
    storage_file_close(app->findmy_file);
    storage_file_free(app->findmy_file);
    app->findmy_file = NULL;
    app->findmy_rows_since_sync = 0;
}

static void wardrive_findmy_log(WardrivingApp* app, const WardriveEntry* entry) {
    if(!app->save_map || !entry->brand[0] || !wardrive_findmy_open(app)) return;

    char prefix[64];
    const int prefix_len = snprintf(
        prefix, sizeof(prefix), "%lu,%s,", (unsigned long)entry->first_seen, entry->brand);
    if(prefix_len <= 0 || (size_t)prefix_len >= sizeof(prefix)) return;
    if(!wardrive_file_write(app->findmy_file, prefix, (size_t)prefix_len) ||
       !wardrive_csv_field(app->findmy_file, entry->name[0] ? entry->name : "(unknown)") ||
       !wardrive_file_write(app->findmy_file, ",", 1) ||
       !wardrive_csv_field(app->findmy_file, entry->id))
        return;

    char suffix[48];
    const int suffix_len = snprintf(
        suffix, sizeof(suffix), ",%d,%d,%u\n", entry->rssi, entry->best_rssi, entry->channel);
    if(suffix_len <= 0 || (size_t)suffix_len >= sizeof(suffix)) return;
    if(!wardrive_file_write(app->findmy_file, suffix, (size_t)suffix_len)) return;

    app->findmy_rows_since_sync++;
    if(app->findmy_rows_since_sync >= WARD_LOG_SYNC_ROWS) {
        storage_file_sync(app->findmy_file);
        app->findmy_rows_since_sync = 0;
    }
}

/* Pure RSSI-weighted centroid so the position math can be host-tested. Weights
 * are linear in signal strength (stronger APs pull the estimate more). */
static bool wardrive_position_estimate(
    const float* lats,
    const float* lons,
    const int8_t* rssi,
    uint16_t count,
    float* out_lat,
    float* out_lon) {
    double weight_sum = 0.0;
    double lat_sum = 0.0;
    double lon_sum = 0.0;
    for(uint16_t i = 0; i < count; ++i) {
        double weight = (double)rssi[i] + 100.0;
        if(weight <= 0.0) weight = 1.0;
        weight_sum += weight;
        lat_sum += weight * (double)lats[i];
        lon_sum += weight * (double)lons[i];
    }
    if(weight_sum <= 0.0) return false;
    *out_lat = (float)(lat_sum / weight_sum);
    *out_lon = (float)(lon_sum / weight_sum);
    return true;
}

/* Offline AP geolocation database: header + fixed records sorted by BSSID.
 * Lookups binary-search the SD file in place so multi-million entry databases
 * do not need to fit in RAM. Built by tools/build_apdb.py. */
static bool wardrive_apdb_open(WardrivingApp* app) {
    if(app->apdb_file) return true;
    if(!app->storage) return false;

    app->apdb_file = storage_file_alloc(app->storage);
    if(!app->apdb_file) return false;

    uint8_t header[WARD_APDB_HEADER];
    if(!storage_file_open(app->apdb_file, WARD_APDB_PATH, FSAM_READ, FSOM_OPEN_EXISTING) ||
       storage_file_read(app->apdb_file, header, sizeof(header)) != sizeof(header) ||
       memcmp(header, WARD_APDB_MAGIC, 8) != 0) {
        storage_file_close(app->apdb_file);
        storage_file_free(app->apdb_file);
        app->apdb_file = NULL;
        return false;
    }
    memcpy(&app->apdb_count, header + 8, sizeof(app->apdb_count));
    FURI_LOG_I(TAG, "AP DB loaded: %lu entries", (unsigned long)app->apdb_count);
    return true;
}

static void wardrive_apdb_close(WardrivingApp* app) {
    if(!app || !app->apdb_file) return;
    storage_file_close(app->apdb_file);
    storage_file_free(app->apdb_file);
    app->apdb_file = NULL;
    app->apdb_count = 0;
}

static bool
    wardrive_apdb_lookup(WardrivingApp* app, const uint8_t bssid[6], float* lat, float* lon) {
    if(!app->apdb_file || app->apdb_count == 0) return false;
    uint32_t lo = 0;
    uint32_t hi = app->apdb_count;
    uint8_t record[WARD_APDB_RECORD];
    while(lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if(!storage_file_seek(app->apdb_file, WARD_APDB_HEADER + mid * WARD_APDB_RECORD, true) ||
           storage_file_read(app->apdb_file, record, sizeof(record)) != sizeof(record))
            return false;
        const int cmp = memcmp(bssid, record, 6);
        if(cmp == 0) {
            int32_t lat_e7 = 0;
            int32_t lon_e7 = 0;
            memcpy(&lat_e7, record + 6, sizeof(lat_e7));
            memcpy(&lon_e7, record + 10, sizeof(lon_e7));
            *lat = (float)lat_e7 / 10000000.0f;
            *lon = (float)lon_e7 / 10000000.0f;
            return true;
        }
        if(cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return false;
}

/* Track file for PC mapping: one row per scan that produced an estimate. */
static bool wardrive_location_open(WardrivingApp* app) {
    if(app->location_file) return true;
    if(!app->storage || storage_sd_status(app->storage) != FSE_OK) return false;
    if(!storage_simply_mkdir(app->storage, WARD_LOG_DIR)) return false;

    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    char path[128];
    snprintf(
        path,
        sizeof(path),
        WARD_LOG_DIR "/location_%04u%02u%02u_%02u%02u%02u.csv",
        now.year,
        now.month,
        now.day,
        now.hour,
        now.minute,
        now.second);

    app->location_file = storage_file_alloc(app->storage);
    if(!app->location_file) return false;
    if(!storage_file_open(app->location_file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(app->location_file);
        app->location_file = NULL;
        return false;
    }
    static const char header[] = "timestamp,latitude,longitude,matched_aps\n";
    if(!wardrive_file_write(app->location_file, header, sizeof(header) - 1) ||
       !storage_file_sync(app->location_file)) {
        storage_file_close(app->location_file);
        storage_file_free(app->location_file);
        app->location_file = NULL;
        return false;
    }
    FURI_LOG_I(TAG, "Location log %s", path);
    return true;
}

static void wardrive_location_close(WardrivingApp* app) {
    if(!app || !app->location_file) return;
    storage_file_sync(app->location_file);
    storage_file_close(app->location_file);
    storage_file_free(app->location_file);
    app->location_file = NULL;
}

static void wardrive_location_log(WardrivingApp* app, float lat, float lon, uint16_t matched) {
    if(!wardrive_location_open(app)) return;
    char row[96];
    const int len = snprintf(
        row,
        sizeof(row),
        "%lu,%.6f,%.6f,%u\n",
        (unsigned long)furi_hal_rtc_get_timestamp(),
        (double)lat,
        (double)lon,
        matched);
    if(len > 0 && (size_t)len < sizeof(row)) {
        wardrive_file_write(app->location_file, row, (size_t)len);
        storage_file_sync(app->location_file);
    }
}

static void wardrive_wifi_locate(
    WardrivingApp* app,
    const wifi_ap_record_t* records,
    uint16_t count) {
    float lats[64];
    float lons[64];
    int8_t rssi[64];
    uint16_t matched = 0;
    for(uint16_t i = 0; i < count && matched < COUNT_OF(lats); ++i) {
        float lat = 0;
        float lon = 0;
        if(!wardrive_apdb_lookup(app, records[i].bssid, &lat, &lon)) continue;
        lats[matched] = lat;
        lons[matched] = lon;
        rssi[matched] = records[i].rssi;
        matched++;
    }
    if(matched == 0) return;

    float lat = 0;
    float lon = 0;
    if(!wardrive_position_estimate(lats, lons, rssi, matched, &lat, &lon)) return;
    app->have_location = true;
    app->last_lat = lat;
    app->last_lon = lon;
    wardrive_location_log(app, lat, lon, matched);
}

static bool wardrive_log_open(WardrivingApp* app) {
    if(!app || !app->storage || storage_sd_status(app->storage) != FSE_OK) return false;
    if(!storage_simply_mkdir(app->storage, WARD_LOG_DIR)) return false;

    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    snprintf(
        app->log_path,
        sizeof(app->log_path),
        WARD_LOG_DIR "/%s_%04u%02u%02u_%02u%02u%02u_%lu.csv",
        wardrive_mode_filename(app->mode),
        now.year,
        now.month,
        now.day,
        now.hour,
        now.minute,
        now.second,
        (unsigned long)furi_get_tick());

    app->log_file = storage_file_alloc(app->storage);
    if(!app->log_file) return false;
    if(!storage_file_open(app->log_file, app->log_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(app->log_file);
        app->log_file = NULL;
        return false;
    }

    static const char header[] =
        "timestamp,mode,name,id,rssi,best_rssi,channel,details,vendor\n";
    if(!wardrive_file_write(app->log_file, header, sizeof(header) - 1) ||
       !storage_file_sync(app->log_file)) {
        storage_file_close(app->log_file);
        storage_file_free(app->log_file);
        app->log_file = NULL;
        return false;
    }
    FURI_LOG_I(TAG, "Logging %s", app->log_path);
    return true;
}

static bool wardrive_log_entry(WardrivingApp* app, const WardriveEntry* entry) {
    if(!app || !app->log_file || !entry) return false;

    char prefix[80];
    const int prefix_len = snprintf(
        prefix,
        sizeof(prefix),
        "%lu,%s,",
        (unsigned long)entry->first_seen,
        wardrive_mode_filename(app->mode));
    if(prefix_len <= 0 || (size_t)prefix_len >= sizeof(prefix) ||
       !wardrive_file_write(app->log_file, prefix, (size_t)prefix_len) ||
       !wardrive_csv_field(app->log_file, entry->name[0] ? entry->name : "(unknown)") ||
       !wardrive_file_write(app->log_file, ",", 1) ||
       !wardrive_csv_field(app->log_file, entry->id)) {
        return false;
    }

    char suffix[96];
    const int suffix_len = snprintf(
        suffix,
        sizeof(suffix),
        ",%d,%d,%u,",
        entry->rssi,
        entry->best_rssi,
        entry->channel);
    if(suffix_len <= 0 || (size_t)suffix_len >= sizeof(suffix) ||
       !wardrive_file_write(app->log_file, suffix, (size_t)suffix_len) ||
       !wardrive_csv_field(app->log_file, entry->details) ||
       !wardrive_file_write(app->log_file, ",", 1) ||
       !wardrive_csv_field(app->log_file, entry->vendor) ||
       !wardrive_file_write(app->log_file, "\n", 1)) {
        return false;
    }

    wardrive_findmy_log(app, entry);

    app->log_rows_since_sync++;
    if(app->log_rows_since_sync >= WARD_LOG_SYNC_ROWS) {
        if(!storage_file_sync(app->log_file)) return false;
        app->log_rows_since_sync = 0;
    }
    return true;
}

static bool wardrive_flush_unlogged(WardrivingApp* app) {
    while(app && app->logged_count < app->count) {
        WardriveEntry copy;
        if(furi_mutex_acquire(app->lock, FuriWaitForever) != FuriStatusOk) return false;
        if(app->logged_count >= app->count) {
            furi_mutex_release(app->lock);
            break;
        }
        copy = app->entries[app->logged_count];
        app->logged_count++;
        furi_mutex_release(app->lock);

        if(!wardrive_log_entry(app, &copy)) {
            app->log_failed = true;
            wardrive_set_status(app, "SD write failed");
            app->stop_requested = true;
            return false;
        }
    }
    return true;
}

static const char* wardrive_wifi_auth_label(wifi_auth_mode_t auth) {
    switch(auth) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-EAP";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    default:
        return "secured";
    }
}

static void wardrive_wifi_record(WardrivingApp* app, const wifi_ap_record_t* ap) {
    WardriveEntry entry = {0};
    size_t ssid_len = strnlen((const char*)ap->ssid, sizeof(ap->ssid));
    if(ssid_len > sizeof(entry.name) - 1) ssid_len = sizeof(entry.name) - 1;
    memcpy(entry.name, ap->ssid, ssid_len);
    entry.name[ssid_len] = '\0';
    wardrive_sanitize_text(entry.name);
    snprintf(
        entry.id,
        sizeof(entry.id),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        ap->bssid[0],
        ap->bssid[1],
        ap->bssid[2],
        ap->bssid[3],
        ap->bssid[4],
        ap->bssid[5]);
    strlcpy(entry.details, wardrive_wifi_auth_label(ap->authmode), sizeof(entry.details));
    const char* vendor = (ap->bssid[0] & 0x03) ? NULL : wlan_oui_lookup(app->oui, ap->bssid);
    if(vendor) snprintf(entry.vendor, sizeof(entry.vendor), "OUI: %.26s", vendor);
    else strlcpy(entry.vendor, (ap->bssid[0] & 0x02) ? "Local MAC / unknown" :
                     (app->oui ? "OUI unknown" : "OUI DB missing"), sizeof(entry.vendor));
    entry.rssi = ap->rssi;
    entry.best_rssi = ap->rssi;
    entry.channel = ap->primary;
    entry.first_seen = furi_hal_rtc_get_timestamp();
    entry.last_seen = entry.first_seen;
    wardrive_add_or_update(app, &entry);
}

static void wardrive_ble_parse_name(
    const uint8_t* data,
    uint8_t data_len,
    char* output,
    size_t output_size) {
    if(!data || !output || output_size == 0) return;
    output[0] = '\0';

    uint8_t pos = 0;
    while(pos + 1 < data_len) {
        const uint8_t field_len = data[pos];
        if(field_len == 0 || pos + field_len >= data_len) break;
        const uint8_t type = data[pos + 1];
        if(type == BLE_HS_ADV_TYPE_INCOMP_NAME || type == BLE_HS_ADV_TYPE_COMP_NAME) {
            size_t name_len = field_len - 1;
            if(name_len >= output_size) name_len = output_size - 1;
            memcpy(output, &data[pos + 2], name_len);
            output[name_len] = '\0';
            wardrive_sanitize_text(output);
            return;
        }
        pos += field_len + 1;
    }
}

static int wardrive_ble_gap_callback(struct ble_gap_event* event, void* context) {
    UNUSED(context);
    WardrivingApp* app = wardrive_ble_owner;
    if(!app || !event) return 0;

    switch(event->type) {
    case BLE_GAP_EVENT_DISC: {
        WardriveEntry entry = {0};
        const struct ble_gap_disc_desc* disc = &event->disc;
        wardrive_ble_parse_name(disc->data, disc->length_data, entry.name, sizeof(entry.name));
        snprintf(
            entry.id,
            sizeof(entry.id),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            disc->addr.val[5],
            disc->addr.val[4],
            disc->addr.val[3],
            disc->addr.val[2],
            disc->addr.val[1],
            disc->addr.val[0]);
        strlcpy(
            entry.details,
            disc->addr.type == BLE_ADDR_PUBLIC ? "public" : "random",
            sizeof(entry.details));
        uint8_t mac[6];
        for(size_t i = 0; i < sizeof(mac); ++i) mac[i] = disc->addr.val[5 - i];
        const char* vendor = disc->addr.type == BLE_ADDR_PUBLIC ?
                                 wlan_oui_lookup(app->oui, mac) : NULL;
        if(vendor) snprintf(entry.vendor, sizeof(entry.vendor), "OUI: %.26s", vendor);
        /* Manufacturer data is an advertised company hint, including on
         * random addresses. It does not identify a phone model. */
        for(size_t pos = 0; pos + 1 < disc->length_data;) {
            const size_t len = disc->data[pos];
            if(!len || pos + len >= disc->length_data) break;
            if(disc->data[pos + 1] == 0xFF && len >= 3) {
                const uint16_t company = disc->data[pos + 2] | (disc->data[pos + 3] << 8);
                if(company == 0x004C) strlcpy(entry.vendor, "Adv: Apple", sizeof(entry.vendor));
                else if(company == 0x0075) strlcpy(entry.vendor, "Adv: Samsung", sizeof(entry.vendor));
            }
            pos += len + 1;
        }
        if(!entry.vendor[0]) {
            strlcpy(entry.vendor, disc->addr.type != BLE_ADDR_PUBLIC ? "Random MAC / unknown" :
                        (app->oui ? "OUI unknown" : "OUI DB missing"), sizeof(entry.vendor));
        }
        entry.rssi = disc->rssi;
        entry.best_rssi = disc->rssi;
        entry.first_seen = furi_hal_rtc_get_timestamp();
        entry.last_seen = entry.first_seen;
        strlcpy(
            entry.brand,
            wardrive_tracker_label(wardrive_tracker_identify(disc->data, disc->length_data)),
            sizeof(entry.brand));
        wardrive_add_or_update(app, &entry);
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        app->ble_scan_started = false;
        if(!app->stop_requested) app->ble_scan_failed = true;
        break;

    default:
        break;
    }
    return 0;
}

/* Shared teardown for whichever NimBLE feature (scan or beacon) currently owns
 * the radio. NimBLE and WiFi share the radio through ESP-IDF coexistence, so
 * WiFi remains running throughout; only the default Bt profile is suspended
 * and restored here. */
static void wardrive_nimble_release(WardrivingApp* app) {
    if(!app) return;
    nimble_glue_stop();
    if(app->bt_was_enabled) {
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
    }
    app->bt_was_enabled = false;
}

/* Shared takeover: suspend the default Bt profile and bring up a bare NimBLE
 * host under our own name, ready for either scanning or beacon advertising. */
static bool wardrive_nimble_take(WardrivingApp* app, const char* name) {
    Bt* bt = furi_record_open(RECORD_BT);
    app->bt_was_enabled = bt_is_enabled(bt);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    furi_delay_ms(50);

    esp_err_t err = nimble_glue_init(name);
    if(err != ESP_OK) goto fail;
    nimble_glue_configure_security(false, false, false, BLE_HS_IO_NO_INPUT_OUTPUT);
    err = nimble_glue_start(NULL, NULL);
    if(err != ESP_OK) goto fail;
    return true;

fail:
    FURI_LOG_E(TAG, "NimBLE takeover failed: %s", esp_err_to_name(err));
    wardrive_nimble_release(app);
    return false;
}

static void wardrive_ble_stack_stop(WardrivingApp* app) {
    if(!app) return;

    if(app->ble_scan_started) {
        ble_gap_disc_cancel();
        for(uint8_t i = 0; i < 20 && app->ble_scan_started; ++i) furi_delay_ms(10);
    }
    wardrive_ble_owner = NULL;
    wardrive_nimble_release(app);
}

static bool wardrive_ble_stack_start(WardrivingApp* app) {
    if(!wardrive_nimble_take(app, "Wardrive BLE")) return false;

    wardrive_ble_owner = app;
    app->ble_scan_started = false;
    app->ble_scan_failed = false;
    struct ble_gap_disc_params scan_params = {
        .itvl = 0x50,
        .window = 0x30,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(
        nimble_glue_own_address_type(), BLE_HS_FOREVER, &scan_params, wardrive_ble_gap_callback, app);
    if(rc != 0) {
        FURI_LOG_E(TAG, "BLE scanner start failed: rc=%d", rc);
        wardrive_ble_owner = NULL;
        wardrive_nimble_release(app);
        return false;
    }
    app->ble_scan_started = true;
    return true;
}

/* Builds the raw advertising payload for the selected emulated tag into
 * `buf` (must hold EXTRA_BEACON_MAX_DATA_SIZE bytes) and returns its length,
 * or 0 if the type has no payload (Google - see wardrive_worker_emulate). */
static uint8_t wardrive_build_emu_payload(WardriveTracker type, uint8_t* buf) {
    if(type == WardriveTrackerAirTag) {
        /* Same static AirTag frame as find_my_flipper's default payload
         * (applications/system/find_my_flipper/findmy_state.c) - no live
         * battery byte, that's cosmetic and not needed for tag recognition. */
        uint8_t* p = buf;
        *p++ = 0x1E; // Length
        *p++ = 0xFF; // Manufacturer Specific Data
        *p++ = 0x4C; // Company ID (Apple, Inc.)
        *p++ = 0x00; // ...
        *p++ = 0x12; // Type (FindMy)
        *p++ = 0x19; // Length
        *p++ = 0x00; // Battery Status set to Full
        for(size_t i = 0; i < 22; ++i) *p++ = 0x00; // Placeholder public key
        *p++ = 0x00; // Version
        *p++ = 0x00; // Hint
        return 31;
    }
    if(type == WardriveTrackerSamsung) {
        return ble_spam_build_samsung_buds(buf, 0xFF, 0xFF, 0xFF);
    }
    return 0;
}

static bool wardrive_emu_beacon_start(WardrivingApp* app) {
    if(!wardrive_nimble_take(app, "Wardrive Emulate")) return false;

    /* furi_hal_bt_extra_beacon_start() requires NimBLE to already be synced;
     * nimble_glue_start() does not guarantee that's immediate. Poll briefly
     * rather than assume, since a false return here is silent (not an
     * error/assert) per components/furi_hal/furi_hal_bt.c. */
    bool synced = false;
    for(uint8_t i = 0; i < 50 && !synced; ++i) {
        synced = nimble_glue_is_synced();
        if(!synced) furi_delay_ms(10);
    }
    if(!synced) {
        FURI_LOG_E(TAG, "NimBLE never synced for beacon");
        wardrive_nimble_release(app);
        return false;
    }

    uint8_t data[EXTRA_BEACON_MAX_DATA_SIZE];
    const uint8_t len = wardrive_build_emu_payload(app->emu_tag_type, data);
    if(len == 0) {
        wardrive_nimble_release(app);
        return false;
    }

    GapExtraBeaconConfig config = {
        .min_adv_interval_ms = 5000,
        .max_adv_interval_ms = 5150,
        .adv_channel_map = GapAdvChannelMapAll,
        .adv_power_level = GapAdvPowerLevel_0dBm + 6,
        .address_type = GapAddressTypePublic,
        .address = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11},
    };
    if(!furi_hal_bt_extra_beacon_set_config(&config) ||
       !furi_hal_bt_extra_beacon_set_data(data, len) || !furi_hal_bt_extra_beacon_start()) {
        FURI_LOG_E(TAG, "Beacon configure/start failed");
        wardrive_nimble_release(app);
        return false;
    }
    return true;
}

static void wardrive_emu_beacon_stop(WardrivingApp* app) {
    if(furi_hal_bt_extra_beacon_is_active()) furi_hal_bt_extra_beacon_stop();
    wardrive_nimble_release(app);
}

static void wardrive_sub_record(WardrivingApp* app, uint32_t frequency, float rssi) {
    WardriveEntry entry = {0};
    strlcpy(entry.name, "RF signal", sizeof(entry.name));
    snprintf(
        entry.id,
        sizeof(entry.id),
        "%lu.%03lu MHz",
        (unsigned long)(frequency / 1000000U),
        (unsigned long)((frequency % 1000000U) / 1000U));
    strlcpy(entry.details, "energy", sizeof(entry.details));
    entry.rssi = (int16_t)rssi;
    entry.best_rssi = (int16_t)rssi;
    entry.frequency = frequency;
    entry.first_seen = furi_hal_rtc_get_timestamp();
    entry.last_seen = entry.first_seen;
    wardrive_add_or_update(app, &entry);
}

typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t step;
} WardriveSubBand;

static const WardriveSubBand wardrive_sub_bands[] = {
    {.start = 281000000U, .end = 361000000U, .step = 500000U},
    {.start = 378000000U, .end = 481000000U, .step = 500000U},
    {.start = 749000000U, .end = 962000000U, .step = 500000U},
};

static void wardrive_worker_wifi(WardrivingApp* app) {
    wardrive_set_status(app, "Starting WiFi...");
    if(!wlan_hal_start()) {
        wardrive_set_status(app, "WiFi start failed");
        return;
    }

    /* Stop background association/reconnect before requesting scans: the
     * driver rejects scans while it is connecting to a saved network. */
    wlan_hal_disconnect();

    wardrive_set_status(app, "Scanning - Back stops");
    while(!app->stop_requested) {
        wifi_ap_record_t* records = NULL;
        uint16_t count = 0;
        const bool scanned = wlan_hal_scan(&records, &count, 64);
        wardrive_set_status(app, scanned ? (count ? "Scanning" : "No APs - retrying") :
                                             "WiFi scan failed - retrying");
        for(uint16_t i = 0; i < count; ++i) wardrive_wifi_record(app, &records[i]);
        wardrive_wifi_locate(app, records, count);
        free(records);
        if(!wardrive_flush_unlogged(app)) break;
        for(uint8_t i = 0; i < 5 && !app->stop_requested; ++i) furi_delay_ms(50);
    }
    wlan_hal_stop();
}

static void wardrive_worker_ble(WardrivingApp* app) {
    wardrive_set_status(app, "Starting BLE...");
    if(!wardrive_ble_stack_start(app)) {
        wardrive_set_status(app, "BLE start failed");
        return;
    }

    wardrive_set_status(app, "Scanning - Back stops");
    while(!app->stop_requested) {
        if(app->ble_scan_failed) {
            wardrive_set_status(app, "BLE scan interrupted");
            break;
        }
        wardrive_flush_unlogged(app);
        furi_delay_ms(200);
    }
    wardrive_flush_unlogged(app);
    wardrive_ble_stack_stop(app);
}

static void wardrive_worker_subghz(WardrivingApp* app) {
    wardrive_set_status(app, "Starting CC1101...");
    furi_hal_subghz_load_custom_preset(subghz_device_cc1101_preset_ook_650khz_async_regs);
    wardrive_set_status(app, "Scanning - Back stops");

    while(!app->stop_requested) {
        for(size_t band_index = 0;
            band_index < COUNT_OF(wardrive_sub_bands) && !app->stop_requested;
            ++band_index) {
            const WardriveSubBand* band = &wardrive_sub_bands[band_index];
            for(uint32_t requested = band->start;
                requested <= band->end && !app->stop_requested;
                requested += band->step) {
                furi_hal_subghz_idle();
                const uint32_t actual = furi_hal_subghz_set_frequency_and_path(requested);
                furi_hal_subghz_rx();
                furi_delay_ms(3);
                float rssi = furi_hal_subghz_get_rssi();
                furi_delay_ms(1);
                const float second = furi_hal_subghz_get_rssi();
                if(second > rssi) rssi = second;
                if(rssi >= WARD_SUB_THRESHOLD_DBM) wardrive_sub_record(app, actual, rssi);
            }
        }
        wardrive_flush_unlogged(app);
    }

    wardrive_flush_unlogged(app);
    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
}

static void wardrive_worker_emulate(WardrivingApp* app) {
    if(app->emu_tag_type == WardriveTrackerGoogle) {
        wardrive_set_status(app, "Google emulation unsupported");
        return;
    }

    wardrive_set_status(app, "Starting beacon...");
    if(!wardrive_emu_beacon_start(app)) {
        wardrive_set_status(app, "Beacon start failed");
        return;
    }

    wardrive_set_status(app, "Emulating - Back stops");
    while(!app->stop_requested) furi_delay_ms(200);
    wardrive_emu_beacon_stop(app);
}

static int32_t wardrive_worker(void* context) {
    WardrivingApp* app = context;
    if(!app) return -1;

    switch(app->mode) {
    case WardriveModeSubGhz:
        wardrive_worker_subghz(app);
        break;
    case WardriveModeBle:
        wardrive_worker_ble(app);
        break;
    case WardriveModeWifi:
        wardrive_worker_wifi(app);
        break;
    case WardriveModeEmulate:
        wardrive_worker_emulate(app);
        break;
    default:
        break;
    }

    app->worker_active = false;
    return 0;
}

static void wardrive_log_close(WardrivingApp* app) {
    if(!app || !app->log_file) return;
    storage_file_sync(app->log_file);
    storage_file_close(app->log_file);
    storage_file_free(app->log_file);
    app->log_file = NULL;
}

static void wardrive_session_stop(WardrivingApp* app) {
    if(!app) return;
    app->stop_requested = true;
    if(app->worker) {
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }
    wardrive_log_close(app);
    wardrive_findmy_close(app);
    wardrive_location_close(app);
    wardrive_apdb_close(app);
    if(app->entries) {
        heap_caps_free(app->entries);
        app->entries = NULL;
    }
    wlan_oui_free(app->oui);
    app->oui = NULL;
    app->worker_active = false;
}

static bool wardrive_session_start(WardrivingApp* app, WardriveMode mode) {
    wardrive_session_stop(app);

    app->entries = heap_caps_calloc(
        WARD_MAX_ENTRIES, sizeof(WardriveEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!app->entries) {
        app->mode = mode;
        wardrive_set_status(app, "PSRAM unavailable");
        return false;
    }

    if(furi_mutex_acquire(app->lock, FuriWaitForever) == FuriStatusOk) {
        app->mode = mode;
        app->count = 0;
        app->logged_count = 0;
        app->scroll = 0;
        app->dropped = 0;
        app->log_rows_since_sync = 0;
        app->findmy_rows_since_sync = 0;
        app->follow_latest = true;
        app->log_failed = false;
        app->status[0] = '\0';
        furi_mutex_release(app->lock);
    }
    app->stop_requested = false;
    if(mode == WardriveModeWifi || mode == WardriveModeBle) app->oui = wlan_oui_load();

    app->have_location = false;
    app->last_lat = 0;
    app->last_lon = 0;
    if(mode == WardriveModeWifi && !wardrive_apdb_open(app)) {
        wardrive_set_status(app, "AP DB missing - no location");
    }

    /* Emulation never logs entries, so skip creating an (empty) session CSV. */
    if(mode != WardriveModeEmulate && !wardrive_log_open(app)) {
        wardrive_set_status(app, "SD card/log failed");
        heap_caps_free(app->entries);
        app->entries = NULL;
        return false;
    }

    app->worker = furi_thread_alloc_ex("WardriveScan", WARD_WORKER_STACK, wardrive_worker, app);
    if(!app->worker) {
        wardrive_set_status(app, "Worker allocation failed");
        wardrive_log_close(app);
        heap_caps_free(app->entries);
        app->entries = NULL;
        return false;
    }

    app->worker_active = true;
    furi_thread_start(app->worker);
    return true;
}

static void wardrive_scan_draw(Canvas* canvas, void* context) {
    const WardriveViewModel* model = context;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    char line[48];
    canvas_set_font(canvas, FontPrimary);
    snprintf(
        line,
        sizeof(line),
        "%s Found: %u%s",
        wardrive_mode_label(model->mode),
        model->found,
        model->dropped ? "+" : "");
    canvas_draw_str(canvas, 1, 11, line);

    canvas_set_font(canvas, FontSecondary);
    if(model->row_count == 0) {
        canvas_draw_str_aligned(
            canvas,
            64,
            34,
            AlignCenter,
            AlignCenter,
            model->status[0] ? model->status : "Starting...");
    } else {
        for(uint8_t i = 0; i < model->row_count; ++i) {
            const WardriveEntry* entry = &model->rows[i];
            if(model->mode == WardriveModeSubGhz) {
                snprintf(line, sizeof(line), "%c %s  %ddBm", i == 0 ? '>' : ' ',
                    entry->name[0] ? entry->name : "(unknown)", entry->rssi);
                canvas_draw_str(canvas, 1, 22 + i * 20, line);
                snprintf(line, sizeof(line), "  %s", entry->id);
                canvas_draw_str(canvas, 1, 31 + i * 20, line);
                continue;
            }
            const uint8_t name_y = 22;
            const uint8_t id_y = 32;
            snprintf(
                line,
                sizeof(line),
                "%c %.20s",
                i == 0 ? '>' : ' ',
                entry->brand[0] ? entry->brand :
                (entry->vendor[0] ?
                     ((strncmp(entry->vendor, "OUI: ", 5) == 0 ||
                       strncmp(entry->vendor, "Adv: ", 5) == 0) ? entry->vendor + 5 : "Unknown brand") :
                     (entry->name[0] ? entry->name : "(unknown)")));
            canvas_draw_str(canvas, 1, name_y, line);
            snprintf(line, sizeof(line), "%s", entry->id);
            canvas_draw_str(canvas, 1, id_y, line);
            snprintf(line, sizeof(line), "%.24s", entry->name[0] ? entry->name : entry->vendor);
            canvas_draw_str(canvas, 1, 42, line);
            snprintf(
                line,
                sizeof(line),
                "%ddBm ch%u %.12s",
                entry->rssi,
                entry->channel,
                entry->brand[0] ? entry->brand : entry->details);
            canvas_draw_str(canvas, 1, 52, line);
        }
    }

    canvas_set_font(canvas, FontSecondary);
    if(model->have_location) {
        snprintf(
            line,
            sizeof(line),
            "%.5f,%.5f",
            (double)model->latitude,
            (double)model->longitude);
        canvas_draw_str_aligned(canvas, 127, 63, AlignRight, AlignBottom, line);
    } else {
        canvas_draw_str_aligned(
            canvas,
            127,
            63,
            AlignRight,
            AlignBottom,
            !model->running || strstr(model->status, "failed") ? model->status :
                                                                 "Up/Down  Back=Stop");
    }
}

static bool wardrive_scan_input(InputEvent* event, void* context) {
    WardrivingApp* app = context;
    if(!app || !event) return false;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    if(event->key == InputKeyUp) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WARD_EVENT_SCROLL_UP);
        return true;
    }
    if(event->key == InputKeyDown) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WARD_EVENT_SCROLL_DOWN);
        return true;
    }
    return false;
}

static void wardrive_refresh_view(WardrivingApp* app) {
    if(!app || !app->scan_view || !app->lock) return;
    WardriveViewModel* model = view_get_model(app->scan_view);
    if(!model) return;

    if(furi_mutex_acquire(app->lock, FuriWaitForever) == FuriStatusOk) {
        model->mode = app->mode;
        model->found = app->count;
        model->dropped = app->dropped;
        model->running = app->worker_active;
        model->have_location = app->have_location;
        model->latitude = app->last_lat;
        model->longitude = app->last_lon;
        strlcpy(model->status, app->status, sizeof(model->status));
        model->row_count = 0;

        if(app->entries && app->count > 0) {
            const uint16_t max_scroll =
                app->count > wardrive_visible_entries(app->mode) ?
                    app->count - wardrive_visible_entries(app->mode) : 0;
            if(app->follow_latest) app->scroll = max_scroll;
            if(app->scroll > max_scroll) app->scroll = max_scroll;
            for(uint8_t i = 0;
                i < wardrive_visible_entries(app->mode) && app->scroll + i < app->count;
                ++i) {
                model->rows[i] = app->entries[app->scroll + i];
                model->row_count++;
            }
        }
        furi_mutex_release(app->lock);
    }
    view_commit_model(app->scan_view, true);
}

static void wardrive_settings_apply_text(VariableItem* item, bool value) {
    variable_item_set_current_value_index(item, value ? 1 : 0);
    variable_item_set_current_value_text(item, value ? "Yes" : "No");
}

static void wardrive_settings_save_map_changed(VariableItem* item) {
    WardrivingApp* app = variable_item_get_context(item);
    app->save_map = variable_item_get_current_value_index(item) != 0;
    wardrive_settings_apply_text(item, app->save_map);
    wardrive_findmy_save(app);
}

static void wardrive_settings_tag_type_changed(VariableItem* item) {
    WardrivingApp* app = variable_item_get_context(item);
    /* Index 0/1/2 -> AirTag/Google/Samsung (WardriveTrackerNone is not offered). */
    app->emu_tag_type = (WardriveTracker)(variable_item_get_current_value_index(item) + 1);
    variable_item_set_current_value_text(item, wardrive_tracker_label(app->emu_tag_type));
    wardrive_findmy_save(app);
}

static void wardrive_settings_enter(WardrivingApp* app) {
    VariableItemList* list = app->settings;
    variable_item_list_reset(list);
    variable_item_list_set_header(list, "FindMy Settings");

    /* AirTag/Google/Samsung tagging is always on during BLE wardriving, so the
     * only choice here is whether tagged hits also get a map row. */
    VariableItem* item = variable_item_list_add(
        list, "Save map on detect", 2, wardrive_settings_save_map_changed, app);
    wardrive_settings_apply_text(item, app->save_map);

    /* Which tag FindMy Emulate broadcasts. Google is listed for symmetry with
     * the detector but is not implemented - see wardrive_worker_emulate(). */
    VariableItem* emu_item = variable_item_list_add(
        list, "Emulate Tag", 3, wardrive_settings_tag_type_changed, app);
    variable_item_set_current_value_index(emu_item, app->emu_tag_type - 1);
    variable_item_set_current_value_text(emu_item, wardrive_tracker_label(app->emu_tag_type));
}

static void wardrive_menu_callback(void* context, uint32_t index) {
    WardrivingApp* app = context;
    if(!app) return;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static bool wardrive_custom_event(void* context, uint32_t event) {
    WardrivingApp* app = context;
    if(!app) return false;

    if(event >= WardriveModeSubGhz && event <= WardriveModeEmulate) {
        app->scan_view_active = true;
        app->mode = (WardriveMode)event;
        wardrive_set_status(app, "Opening SD log...");
        wardrive_refresh_view(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, WARD_VIEW_SCAN);
        wardrive_session_start(app, (WardriveMode)event);
        wardrive_refresh_view(app);
        return true;
    }

    if(event == WARD_EVENT_SETTINGS) {
        app->settings_view_active = true;
        wardrive_settings_enter(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, WARD_VIEW_SETTINGS);
        return true;
    }

    if(event == WARD_EVENT_SCROLL_UP || event == WARD_EVENT_SCROLL_DOWN) {
        if(furi_mutex_acquire(app->lock, FuriWaitForever) == FuriStatusOk) {
            const uint16_t max_scroll =
                app->count > wardrive_visible_entries(app->mode) ?
                    app->count - wardrive_visible_entries(app->mode) : 0;
            if(event == WARD_EVENT_SCROLL_UP) {
                app->follow_latest = false;
                if(app->scroll > 0) app->scroll--;
            } else if(app->scroll < max_scroll) {
                app->scroll++;
                app->follow_latest = app->scroll == max_scroll;
            } else {
                app->follow_latest = true;
            }
            furi_mutex_release(app->lock);
        }
        wardrive_refresh_view(app);
        return true;
    }
    return false;
}

static bool wardrive_back_event(void* context) {
    WardrivingApp* app = context;
    if(!app) return false;
    if(app->settings_view_active) {
        app->settings_view_active = false;
        view_dispatcher_switch_to_view(app->view_dispatcher, WARD_VIEW_MENU);
        return true;
    }
    if(!app->scan_view_active) return false;

    wardrive_session_stop(app);
    app->scan_view_active = false;
    app->mode = WardriveModeNone;
    view_dispatcher_switch_to_view(app->view_dispatcher, WARD_VIEW_MENU);
    return true;
}

static void wardrive_tick_event(void* context) {
    WardrivingApp* app = context;
    if(app && app->scan_view_active) wardrive_refresh_view(app);
}

static WardrivingApp* wardrive_app_alloc(void) {
    WardrivingApp* app = calloc(1, sizeof(WardrivingApp));
    if(!app) return NULL;

    app->lock = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!app->lock) {
        free(app);
        return NULL;
    }

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->view_dispatcher = view_dispatcher_alloc();
    app->submenu = submenu_alloc();
    app->scan_view = view_alloc();
    app->settings = variable_item_list_alloc();
    if(!app->gui || !app->storage || !app->view_dispatcher || !app->submenu || !app->scan_view ||
       !app->settings) {
        return app;
    }

    wardrive_findmy_load(app);

    submenu_set_header(app->submenu, "Wardriving");
    submenu_add_item(
        app->submenu, "Sub-GHz Wardriving", WardriveModeSubGhz, wardrive_menu_callback, app);
    submenu_add_item(app->submenu, "BLE Wardriving", WardriveModeBle, wardrive_menu_callback, app);
    /* WiFi Wardriving stays hidden from the menu: it's the one mode that turns
     * WiFi on, and this board has a real, unresolved WiFi-vs-Bluetooth memory
     * conflict (BLE controller init OOMs while WiFi is running - see project
     * notes). The offline apdb.bin location feature was entirely fed by this
     * mode's own WiFi scan, so it goes dark with it; detection and emulation
     * below are pure BLE and unaffected. Code stays in place, same as before. */
    submenu_add_item(
        app->submenu, "FindMy Emulate", WardriveModeEmulate, wardrive_menu_callback, app);
    submenu_add_item(app->submenu, "FindMy Settings", WARD_EVENT_SETTINGS, wardrive_menu_callback, app);

    view_allocate_model(app->scan_view, ViewModelTypeLocking, sizeof(WardriveViewModel));
    view_set_context(app->scan_view, app);
    view_set_draw_callback(app->scan_view, wardrive_scan_draw);
    view_set_input_callback(app->scan_view, wardrive_scan_input);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, wardrive_custom_event);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, wardrive_back_event);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, wardrive_tick_event, 250);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_add_view(app->view_dispatcher, WARD_VIEW_MENU, submenu_get_view(app->submenu));
    app->menu_view_added = true;
    view_dispatcher_add_view(app->view_dispatcher, WARD_VIEW_SCAN, app->scan_view);
    app->scan_view_added = true;
    view_dispatcher_add_view(
        app->view_dispatcher, WARD_VIEW_SETTINGS, variable_item_list_get_view(app->settings));
    app->settings_view_added = true;
    return app;
}

static bool wardrive_app_ready(const WardrivingApp* app) {
    return app && app->lock && app->gui && app->storage && app->view_dispatcher && app->submenu &&
           app->scan_view && app->settings;
}

static void wardrive_app_free(WardrivingApp* app) {
    if(!app) return;
    wardrive_session_stop(app);

    if(app->view_dispatcher) {
        if(app->menu_view_added) view_dispatcher_remove_view(app->view_dispatcher, WARD_VIEW_MENU);
        if(app->scan_view_added) view_dispatcher_remove_view(app->view_dispatcher, WARD_VIEW_SCAN);
        if(app->settings_view_added)
            view_dispatcher_remove_view(app->view_dispatcher, WARD_VIEW_SETTINGS);
    }
    if(app->submenu) submenu_free(app->submenu);
    if(app->scan_view) view_free(app->scan_view);
    if(app->settings) variable_item_list_free(app->settings);
    if(app->view_dispatcher) view_dispatcher_free(app->view_dispatcher);
    if(app->storage) furi_record_close(RECORD_STORAGE);
    if(app->gui) furi_record_close(RECORD_GUI);
    if(app->lock) furi_mutex_free(app->lock);
    free(app);
}

int32_t wardriving_app(void* args) {
    UNUSED(args);
    WardrivingApp* app = wardrive_app_alloc();
    if(!wardrive_app_ready(app)) {
        wardrive_app_free(app);
        return -1;
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, WARD_VIEW_MENU);
    view_dispatcher_run(app->view_dispatcher);
    wardrive_app_free(app);
    return 0;
}
