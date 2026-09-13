#include "ble_detector_parse.h"

#include <furi.h>
#include <btshim.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <esp_heap_caps.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <nimble_glue.h>
#include <wifi/wlan_hal.h>
#include <stdio.h>
#include <string.h>

/* Starting sizes only: both lists grow on demand in PSRAM, so a busy room is
 * no longer truncated at a fixed cap. The uint16_t counters are the real limit. */
#define DETECTOR_RESULTS_CHUNK 32
#define DETECTOR_RAW_CHUNK 96
#define DETECTOR_COUNT_MAX 0xfffeu
#define DETECTOR_EVENT_SELECT 1u
#define DETECTOR_EVENT_UP 2u
#define DETECTOR_EVENT_DOWN 3u
#define DETECTOR_EVENT_TOGGLE 4u

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    uint32_t last_seen;
    uint32_t kinds;
    char name[32];
} DetectorRecord;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    uint32_t last_seen;
    uint8_t adv[31];
    uint8_t adv_len;
    uint8_t scan_rsp[31];
    uint8_t scan_rsp_len;
} DetectorRawDevice;

typedef struct {
    Gui* gui;
    ViewDispatcher* dispatcher;
    Submenu* menu;
    View* results_view;
    bool radio_ready;
    bool bt_was_enabled;
    bool scanning;
    bool in_results;
    DetectorKind filter;
    uint16_t selected;
    DetectorRecord* records[DetectorKindCount];
    uint16_t counts[DetectorKindCount];
    uint16_t caps[DetectorKindCount];
    DetectorRawDevice* raw;
    uint16_t raw_count;
    uint16_t raw_cap;
    /* The GAP callback appends on the NimBLE task while the app tick walks the
     * same array; a realloc without this could move it mid-iteration. */
    FuriMutex* raw_mutex;
} BleDetectorApp;

static BleDetectorApp* detector_app;

/* Grow a scan list in place: PSRAM first, internal heap as fallback, mirroring
 * how the app struct itself is allocated. On failure the existing buffer is
 * kept and the caller just stops recording new devices. */
static bool
    detector_grow(void** buffer, uint16_t* cap, uint16_t needed, size_t item, uint16_t chunk) {
    if(needed <= *cap) return true;
    uint32_t next = *cap ? (uint32_t)*cap * 2u : chunk;
    while(next < needed) next *= 2u;
    if(next > DETECTOR_COUNT_MAX) next = DETECTOR_COUNT_MAX;
    if(needed > next) return false;
    const size_t bytes = (size_t)next * item;
    void* grown = heap_caps_realloc(*buffer, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!grown) grown = heap_caps_realloc(*buffer, bytes, MALLOC_CAP_8BIT);
    if(!grown) return false;
    memset((uint8_t*)grown + (size_t)*cap * item, 0, bytes - (size_t)*cap * item);
    *buffer = grown;
    *cap = (uint16_t)next;
    return true;
}

static int detector_gap_event(struct ble_gap_event* event, void* context) {
    BleDetectorApp* app = context;
    if(event->type == BLE_GAP_EVENT_DISC) {
        const struct ble_gap_disc_desc* disc = &event->disc;
        uint8_t display_addr[6];
        for(size_t i = 0; i < 6; ++i) display_addr[i] = disc->addr.val[5 - i];
        int found = -1;
        furi_mutex_acquire(app->raw_mutex, FuriWaitForever);
        for(uint16_t i = 0; i < app->raw_count; ++i) {
            if(!memcmp(app->raw[i].addr, display_addr, 6) &&
               app->raw[i].addr_type == disc->addr.type) {
                found = (int)i;
                break;
            }
        }
        if(found < 0) {
            if(!detector_grow(
                   (void**)&app->raw,
                   &app->raw_cap,
                   app->raw_count + 1,
                   sizeof(DetectorRawDevice),
                   DETECTOR_RAW_CHUNK)) {
                furi_mutex_release(app->raw_mutex);
                return 0;
            }
            found = app->raw_count++;
            memset(&app->raw[found], 0, sizeof(DetectorRawDevice));
            memcpy(app->raw[found].addr, display_addr, 6);
            app->raw[found].addr_type = disc->addr.type;
        }
        DetectorRawDevice* raw = &app->raw[found];
        raw->rssi = disc->rssi;
        raw->last_seen = furi_get_tick();
        uint8_t length = disc->length_data > 31 ? 31 : disc->length_data;
        if(disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
            raw->scan_rsp_len = length;
            memcpy(raw->scan_rsp, disc->data, length);
        } else {
            raw->adv_len = length;
            memcpy(raw->adv, disc->data, length);
        }
        furi_mutex_release(app->raw_mutex);
    } else if(event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        detector_app->scanning = false;
    }
    return 0;
}

static bool detector_radio_start(BleDetectorApp* app) {
    wlan_hal_yield_for_memory();
    Bt* bt = furi_record_open(RECORD_BT);
    app->bt_was_enabled = bt_is_enabled(bt);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    furi_delay_ms(50);
    if(nimble_glue_init("BLE Detector") != ESP_OK) {
        if(app->bt_was_enabled) {
            Bt* restore = furi_record_open(RECORD_BT);
            bt_start_stack(restore);
            furi_record_close(RECORD_BT);
        }
        return false;
    }
    nimble_glue_configure_security(false, false, false, BLE_HS_IO_NO_INPUT_OUTPUT);
    if(nimble_glue_start(NULL, NULL) != ESP_OK) {
        nimble_glue_stop();
        if(app->bt_was_enabled) {
            Bt* restore = furi_record_open(RECORD_BT);
            bt_start_stack(restore);
            furi_record_close(RECORD_BT);
        }
        return false;
    }
    furi_mutex_acquire(app->raw_mutex, FuriWaitForever);
    if(app->raw && app->raw_cap)
        memset(app->raw, 0, (size_t)app->raw_cap * sizeof(DetectorRawDevice));
    app->raw_count = 0;
    furi_mutex_release(app->raw_mutex);
    return true;
}

static bool detector_scan_start(BleDetectorApp* app) {
    struct ble_gap_disc_params params = {
        .passive = 0,
        .itvl = 0x50,
        .window = 0x30,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(nimble_glue_own_address_type(), BLE_HS_FOREVER, &params,
                          detector_gap_event, app);
    if(rc != 0) return false;
    app->scanning = true;
    return true;
}

static void detector_scan_stop(BleDetectorApp* app) {
    if(!app->scanning) return;
    ble_gap_disc_cancel();
    for(uint8_t i = 0; i < 20 && app->scanning; ++i) furi_delay_ms(10);
    app->scanning = false;
}

static void detector_radio_stop(BleDetectorApp* app) {
    detector_scan_stop(app);
    if(app->radio_ready) {
        nimble_glue_stop();
        if(app->bt_was_enabled) {
            Bt* bt = furi_record_open(RECORD_BT);
            bt_start_stack(bt);
            furi_record_close(RECORD_BT);
        }
    }
    app->radio_ready = false;
    app->bt_was_enabled = false;
    wlan_hal_resume_user_radio();
}

static const char* detector_distance(int8_t rssi) {
    if(rssi >= -45) return "VERY CLOSE";
    if(rssi >= -60) return "NEAR";
    if(rssi >= -75) return "NEARBY";
    return "FAR";
}

static void detector_menu_callback(void* context, uint32_t index) {
    BleDetectorApp* app = context;
    app->filter = (DetectorKind)index;
    app->in_results = true;
    app->selected = 0;
    view_dispatcher_switch_to_view(app->dispatcher, 1);
    if(!app->radio_ready) {
        app->radio_ready = detector_radio_start(app);
    }
    if(app->radio_ready && detector_scan_start(app)) app->scanning = true;
}

static bool detector_parse_device(DetectorRawDevice* device, DetectorMatch* match) {
    DetectorMatch first = {0};
    DetectorMatch second = {0};
    if(device->adv_len && !ble_detector_parse(device->adv, device->adv_len, &first)) {
        memset(&first, 0, sizeof(first));
    }
    if(device->scan_rsp_len && !ble_detector_parse(device->scan_rsp, device->scan_rsp_len, &second)) {
        memset(&second, 0, sizeof(second));
    }
    memset(match, 0, sizeof(*match));
    match->kinds = first.kinds | second.kinds;
    match->signatures = first.signatures | second.signatures;
    if(first.name[0]) strncpy(match->name, first.name, sizeof(match->name) - 1);
    else if(second.name[0]) strncpy(match->name, second.name, sizeof(match->name) - 1);
    return match->kinds != 0;
}

static void detector_store(DetectorKind kind, DetectorRawDevice* device, DetectorMatch* match) {
    uint16_t count = detector_app->counts[kind];
    int found = -1;
    for(uint16_t i = 0; i < count; ++i) {
        if(!memcmp(detector_app->records[kind][i].addr, device->addr, 6)) {
            found = (int)i;
            break;
        }
    }
    if(found < 0) {
        if(!detector_grow(
               (void**)&detector_app->records[kind],
               &detector_app->caps[kind],
               count + 1,
               sizeof(DetectorRecord),
               DETECTOR_RESULTS_CHUNK))
            return;
        found = count++;
        memset(&detector_app->records[kind][found], 0, sizeof(DetectorRecord));
        memcpy(detector_app->records[kind][found].addr, device->addr, 6);
        detector_app->records[kind][found].addr_type = device->addr_type;
    }
    DetectorRecord* record = &detector_app->records[kind][found];
    record->rssi = device->rssi;
    record->last_seen = device->last_seen;
    record->kinds = match->kinds;
    if(match->name[0]) strncpy(record->name, match->name, sizeof(record->name) - 1);
    detector_app->counts[kind] = count;
}

static void detector_sync_results(BleDetectorApp* app) {
    if(!app->radio_ready || !app->scanning) return;
    /* Held across the walk because the GAP callback may grow (and therefore
     * move) raw[] from the NimBLE task at any moment. */
    furi_mutex_acquire(app->raw_mutex, FuriWaitForever);
    for(uint16_t i = 0; i < app->raw_count; ++i) {
        DetectorMatch match;
        if(!detector_parse_device(&app->raw[i], &match)) continue;
        for(DetectorKind kind = DetectorFlipper; kind < DetectorKindCount; ++kind) {
            if(match.kinds & DETECTOR_BIT(kind)) detector_store(kind, &app->raw[i], &match);
        }
    }
    furi_mutex_release(app->raw_mutex);
    // All Scan is a separate list containing one row per detected category.
    for(DetectorKind kind = DetectorFlipper; kind < DetectorKindCount; ++kind) {
        uint16_t source_count = app->counts[kind];
        for(uint16_t i = 0; i < source_count; ++i) {
            DetectorRecord* source = &app->records[kind][i];
            uint16_t all_count = app->counts[DetectorAll];
            int found = -1;
            for(uint16_t j = 0; j < all_count; ++j) {
                DetectorRecord* row = &app->records[DetectorAll][j];
                if(row->kinds == DETECTOR_BIT(kind) && !memcmp(row->addr, source->addr, 6)) {
                    found = (int)j;
                    break;
                }
            }
            if(found < 0 &&
               detector_grow(
                   (void**)&app->records[DetectorAll],
                   &app->caps[DetectorAll],
                   all_count + 1,
                   sizeof(DetectorRecord),
                   DETECTOR_RESULTS_CHUNK)) {
                found = all_count++;
                memset(&app->records[DetectorAll][found], 0, sizeof(DetectorRecord));
                memcpy(app->records[DetectorAll][found].addr, source->addr, 6);
                app->records[DetectorAll][found].addr_type = source->addr_type;
            }
            if(found >= 0) {
                DetectorRecord* row = &app->records[DetectorAll][found];
                row->rssi = source->rssi;
                row->last_seen = source->last_seen;
                row->kinds = DETECTOR_BIT(kind);
                strncpy(row->name, source->name, sizeof(row->name) - 1);
            }
            app->counts[DetectorAll] = all_count;
        }
    }
}

static DetectorRecord* detector_selected(BleDetectorApp* app) {
    uint16_t count = app->counts[app->filter];
    return count && app->selected < count ? &app->records[app->filter][app->selected] : NULL;
}

static const char* detector_record_label(BleDetectorApp* app, DetectorRecord* record) {
    if(app->filter != DetectorAll) return detector_labels[app->filter];
    for(DetectorKind kind = DetectorFlipper; kind < DetectorKindCount; ++kind) {
        if(record->kinds == DETECTOR_BIT(kind)) return detector_labels[kind];
    }
    return "Detected device";
}

static void detector_draw(Canvas* canvas, void* model) {
    BleDetectorApp* app = *(BleDetectorApp**)model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    char line[48];
    snprintf(line, sizeof(line), "THE FOUND: %u", app->counts[app->filter]);
    canvas_draw_str(canvas, 1, 9, line);
    canvas_set_font(canvas, FontSecondary);
    DetectorRecord* record = detector_selected(app);
    canvas_draw_str(canvas, 1, 19, record ? detector_record_label(app, record) :
                    (app->filter == DetectorAll ? "All Scan" : detector_labels[app->filter]));
    if(record) {
        snprintf(line, sizeof(line), "%u/%u %s", app->selected + 1, app->counts[app->filter],
                 detector_distance(record->rssi));
        canvas_draw_str(canvas, 1, 29, line);
        snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X", record->addr[0], record->addr[1],
                 record->addr[2], record->addr[3], record->addr[4], record->addr[5]);
        canvas_draw_str(canvas, 1, 40, line);
        snprintf(line, sizeof(line), "%d dBm  %lus ago", record->rssi,
                 (unsigned long)((furi_get_tick() - record->last_seen) / 1000));
        canvas_draw_str(canvas, 1, 50, line);
        if(record->name[0]) canvas_draw_str(canvas, 1, 60, record->name);
    } else {
        canvas_draw_str(canvas, 1, 35, app->scanning ? "Scanning..." : "Paused");
        canvas_draw_str(canvas, 1, 52, "OK resume  Up/Down browse");
    }
}

static bool detector_input(InputEvent* event, void* context) {
    BleDetectorApp* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    if(event->key == InputKeyUp || event->key == InputKeyDown || event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(app->dispatcher,
            event->key == InputKeyUp ? DETECTOR_EVENT_UP :
            event->key == InputKeyDown ? DETECTOR_EVENT_DOWN : DETECTOR_EVENT_TOGGLE);
        return true;
    }
    return false;
}

static bool detector_back(void* context) {
    BleDetectorApp* app = context;
    if(!app->in_results) return false;
    detector_scan_stop(app);
    app->in_results = false;
    view_dispatcher_switch_to_view(app->dispatcher, 0);
    return true;
}

static bool detector_custom_event(void* context, uint32_t event) {
    BleDetectorApp* app = context;
    uint16_t count = app->counts[app->filter];
    if(event == DETECTOR_EVENT_UP && count) app->selected = app->selected ? app->selected - 1 : count - 1;
    else if(event == DETECTOR_EVENT_DOWN && count) app->selected = (app->selected + 1) % count;
    else if(event == DETECTOR_EVENT_TOGGLE) {
        if(app->scanning) {
            detector_scan_stop(app);
        } else if(app->radio_ready && detector_scan_start(app)) {
            app->scanning = true;
        }
    }
    view_commit_model(app->results_view, true);
    return true;
}

static void detector_tick(void* context) {
    BleDetectorApp* app = context;
    detector_sync_results(app);
    uint16_t count = app->counts[app->filter];
    if(count && app->selected >= count) app->selected = count - 1;
    view_commit_model(app->results_view, true);
}

int32_t ble_detector_app(void* args) {
    UNUSED(args);
    BleDetectorApp* app = heap_caps_calloc(1, sizeof(*app), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!app) app = calloc(1, sizeof(*app));
    furi_check(app);
    app->raw_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(app->raw_mutex);
    detector_app = app;
    app->filter = DetectorAll;
    app->gui = furi_record_open(RECORD_GUI);
    app->dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->dispatcher, detector_custom_event);
    view_dispatcher_set_navigation_event_callback(app->dispatcher, detector_back);
    view_dispatcher_set_tick_event_callback(app->dispatcher, detector_tick, 500);
    view_dispatcher_attach_to_gui(app->dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->menu = submenu_alloc();
    view_dispatcher_add_view(app->dispatcher, 0, submenu_get_view(app->menu));
    for(DetectorKind kind = DetectorAll; kind < DetectorKindCount; ++kind)
        submenu_add_item(app->menu, detector_labels[kind], kind, detector_menu_callback, app);

    app->results_view = view_alloc();
    view_allocate_model(app->results_view, ViewModelTypeLockFree, sizeof(BleDetectorApp*));
    *(BleDetectorApp**)view_get_model(app->results_view) = app;
    view_commit_model(app->results_view, false);
    view_set_context(app->results_view, app);
    view_set_draw_callback(app->results_view, detector_draw);
    view_set_input_callback(app->results_view, detector_input);
    view_dispatcher_add_view(app->dispatcher, 1, app->results_view);

    view_dispatcher_switch_to_view(app->dispatcher, 0);
    view_dispatcher_run(app->dispatcher);
    detector_radio_stop(app);
    detector_app = NULL;
    view_dispatcher_remove_view(app->dispatcher, 0);
    view_dispatcher_remove_view(app->dispatcher, 1);
    submenu_free(app->menu);
    view_free(app->results_view);
    view_dispatcher_free(app->dispatcher);
    furi_record_close(RECORD_GUI);
    for(DetectorKind kind = DetectorAll; kind < DetectorKindCount; ++kind)
        heap_caps_free(app->records[kind]);
    heap_caps_free(app->raw);
    furi_mutex_free(app->raw_mutex);
    heap_caps_free(app);
    return 0;
}
