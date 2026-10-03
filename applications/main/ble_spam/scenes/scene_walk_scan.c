#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../views/ble_walk_scan_view.h"

#include <esp_log.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>

#define TAG "BleWalk"

enum {
    WalkScanEventFilter = 0x7210,
    WalkScanEventExportCsv,
    WalkScanEventToggleWatch,
};

#define WALK_WATCHLIST_PATH "/ext/ble/watchlist.txt"
#define WALK_SCAN_CSV_PATH "/ext/ble/scan.csv"

static uint8_t s_filter_mode;
static BleWalkAddress s_watch_addresses[BLE_WALK_MAX_DEVICES];
static uint8_t s_watch_count;

static bool walk_watchlist_read(BleWalkAddress* addresses, uint8_t* count) {
    *count = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool opened = storage_file_open(
        file, WALK_WATCHLIST_PATH, FSAM_READ, FSOM_OPEN_EXISTING);
    if(opened) {
        char contents[BLE_WALK_MAX_DEVICES * 20 + 1];
        uint16_t size = storage_file_read(file, contents, sizeof(contents) - 1);
        contents[size] = '\0';
        char* line = contents;
        while(*count < BLE_WALK_MAX_DEVICES && line && *line) {
            unsigned int b[6];
            if(sscanf(
                   line,
                   "%2x:%2x:%2x:%2x:%2x:%2x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
                for(uint8_t i = 0; i < 6; ++i) addresses[*count][i] = (uint8_t)b[i];
                (*count)++;
            }
            line = strchr(line, '\n');
            if(line) line++;
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return opened;
}

static bool walk_watchlist_write(const BleWalkAddress* addresses, uint8_t count) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, "/ext/ble");
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(
        file, WALK_WATCHLIST_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    for(uint8_t i = 0; ok && i < count; ++i) {
        char line[20];
        int length = snprintf(
            line,
            sizeof(line),
            "%02X:%02X:%02X:%02X:%02X:%02X\n",
            addresses[i][0],
            addresses[i][1],
            addresses[i][2],
            addresses[i][3],
            addresses[i][4],
            addresses[i][5]);
        ok = length > 0 && storage_file_write(file, line, (size_t)length) == (size_t)length;
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static bool walk_watchlist_contains(
    const BleWalkAddress* addresses,
    uint8_t count,
    const BleWalkAddress address) {
    for(uint8_t i = 0; i < count; ++i) {
        if(memcmp(addresses[i], address, sizeof(BleWalkAddress)) == 0) return true;
    }
    return false;
}

static void walk_scan_publish(
    BleSpamApp* app,
    const BleWalkDevice* devices,
    uint16_t source_count) {
    BleWalkScanModel* model = view_get_model(app->view_walk_scan);
    const uint8_t previous_source = model->selected < model->count ?
                                    model->source_indices[model->selected] : UINT8_MAX;
    model->count = 0;
    model->selected = 0;
    model->window_offset = 0;
    for(uint16_t i = 0; i < source_count && i < BLE_WALK_MAX_DEVICES; ++i) {
        const bool is_watched =
            walk_watchlist_contains(s_watch_addresses, s_watch_count, devices[i].addr);
        const bool include = (s_filter_mode == 0) ||
                             (s_filter_mode == 1 && devices[i].name[0]) ||
                             (s_filter_mode == 2 && devices[i].rssi >= -65) ||
                             (s_filter_mode == 3 && is_watched);
        if(!include) continue;
        const uint16_t visible = model->count++;
        model->devices[visible] = devices[i];
        model->source_indices[visible] = (uint8_t)i;
        model->watchlisted[visible] = is_watched;
        if(i == previous_source) model->selected = visible;
    }
    if(model->selected >= model->count) model->selected = model->count ? model->count - 1 : 0;
    if(model->selected < model->window_offset) model->window_offset = model->selected;
    if(model->selected >= model->window_offset + WALK_SCAN_ITEMS_ON_SCREEN)
        model->window_offset = model->selected - WALK_SCAN_ITEMS_ON_SCREEN + 1;
    view_commit_model(app->view_walk_scan, true);
}

static void walk_scan_toggle_watch(BleSpamApp* app) {
    BleWalkScanModel* model = view_get_model(app->view_walk_scan);
    if(model->selected >= model->count) return;
    const uint8_t source_index = model->source_indices[model->selected];
    uint16_t device_count = 0;
    BleWalkDevice* devices = ble_walk_hal_get_devices(&device_count);
    if(source_index >= device_count) return;

    const BleWalkAddress* address = &devices[source_index].addr;
    uint8_t count = s_watch_count;
    uint8_t found = count;
    for(uint8_t i = 0; i < count; ++i) {
        if(memcmp(s_watch_addresses[i], *address, sizeof(BleWalkAddress)) == 0) {
            found = i;
            break;
        }
    }
    if(found < count) {
        memmove(
            &s_watch_addresses[found],
            &s_watch_addresses[found + 1],
            (count - found - 1) * sizeof(*s_watch_addresses));
        count--;
    } else if(count < BLE_WALK_MAX_DEVICES) {
        memcpy(s_watch_addresses[count++], *address, sizeof(BleWalkAddress));
    }

    const bool saved = walk_watchlist_write(s_watch_addresses, count);
    if(saved) {
        s_watch_count = count;
    } else {
        walk_watchlist_read(s_watch_addresses, &s_watch_count);
    }
    model->connect_status = saved ? WalkScanStatusSaved : WalkScanStatusFailed;
    if(saved) walk_scan_publish(app, devices, device_count);
    else view_commit_model(app->view_walk_scan, true);
}

static bool walk_scan_export_csv(const BleWalkDevice* devices, uint16_t count) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, "/ext/ble");
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, WALK_SCAN_CSV_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    static const char header[] = "address,name,rssi_dbm\n";
    if(ok) ok = storage_file_write(file, header, sizeof(header) - 1) == sizeof(header) - 1;
    for(uint16_t i = 0; ok && i < count; ++i) {
        char line[96];
        char name[32];
        strlcpy(name, devices[i].name, sizeof(name));
        for(size_t j = 0; name[j]; ++j) {
            if(name[j] == ',' || name[j] == '"' || name[j] == '\r' || name[j] == '\n') name[j] = '_';
        }
        int length = snprintf(
            line,
            sizeof(line),
            "%02X:%02X:%02X:%02X:%02X:%02X,\"%s\",%d\n",
            devices[i].addr[0],
            devices[i].addr[1],
            devices[i].addr[2],
            devices[i].addr[3],
            devices[i].addr[4],
            devices[i].addr[5],
            name,
            devices[i].rssi);
        ok = length > 0 && storage_file_write(file, line, (size_t)length) == (size_t)length;
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

void ble_spam_scene_walk_scan_on_enter(void* context) {
    BleSpamApp* app = context;

    BleWalkScanModel* model = view_get_model(app->view_walk_scan);
    memset(model, 0, sizeof(BleWalkScanModel));
    model->filter = s_filter_mode;
    model->scanning = true;
    model->connect_status = WalkScanStatusNone;
    view_commit_model(app->view_walk_scan, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewWalkScan);

    if(!ble_walk_hal_start()) {
        ESP_LOGE(TAG, "BLE Walk HAL init failed");
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    walk_watchlist_read(s_watch_addresses, &s_watch_count);

    // Disconnect if still connected from previous session
    if(ble_walk_hal_is_connected()) {
        ble_walk_hal_disconnect();
    }

    // Always (re)start scanning
    ble_walk_hal_start_scan();
}

bool ble_spam_scene_walk_scan_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WalkScanEventFilter) {
            s_filter_mode = (s_filter_mode + 1) % 4;
            BleWalkScanModel* model = view_get_model(app->view_walk_scan);
            model->filter = s_filter_mode;
            uint16_t count = 0;
            BleWalkDevice* devices = ble_walk_hal_get_devices(&count);
            walk_scan_publish(app, devices, count);
            consumed = true;
        } else if(event.event == WalkScanEventExportCsv) {
            uint16_t count = 0;
            BleWalkDevice* devices = ble_walk_hal_get_devices(&count);
            BleWalkScanModel* model = view_get_model(app->view_walk_scan);
            model->connect_status = ((count == 0 || devices) &&
                                     walk_scan_export_csv(devices, count)) ?
                                        WalkScanStatusSaved : WalkScanStatusFailed;
            view_commit_model(app->view_walk_scan, true);
            consumed = true;
        } else if(event.event == WalkScanEventToggleWatch) {
            walk_scan_toggle_watch(app);
            consumed = true;
        } else if(event.event == InputKeyUp) {
            BleWalkScanModel* model = view_get_model(app->view_walk_scan);
            if(model->selected > 0) {
                model->selected--;
                if(model->selected < model->window_offset)
                    model->window_offset = model->selected;
            }
            view_commit_model(app->view_walk_scan, true);
            consumed = true;
        } else if(event.event == InputKeyDown) {
            BleWalkScanModel* model = view_get_model(app->view_walk_scan);
            if(model->count > 0 && model->selected < model->count - 1) {
                model->selected++;
                if(model->selected >= model->window_offset + WALK_SCAN_ITEMS_ON_SCREEN)
                    model->window_offset = model->selected - WALK_SCAN_ITEMS_ON_SCREEN + 1;
            }
            view_commit_model(app->view_walk_scan, true);
            consumed = true;
        } else if(event.event == InputKeyOk) {
            BleWalkScanModel* model = view_get_model(app->view_walk_scan);
            uint16_t selected = model->selected < model->count ?
                                    model->source_indices[model->selected] : UINT16_MAX;
            uint16_t dev_count = model->count;
            if(dev_count > 0) {
                model->connect_status = WalkScanStatusConnecting;
            }
            view_commit_model(app->view_walk_scan, true);

            if(dev_count > 0) {
                app->walk_selected_device = selected;
                uint16_t count;
                BleWalkDevice* devices = ble_walk_hal_get_devices(&count);
                if(selected < count) {
                    if(ble_walk_hal_connect(&devices[selected], NULL)) {
                        // Show "Connected!" briefly
                        BleWalkScanModel* m = view_get_model(app->view_walk_scan);
                        m->connect_status = WalkScanStatusConnected;
                        view_commit_model(app->view_walk_scan, true);
                        furi_delay_ms(500);
                        scene_manager_next_scene(app->scene_manager, BleSpamSceneWalkServices);
                    } else {
                        BleWalkScanModel* m = view_get_model(app->view_walk_scan);
                        m->connect_status = WalkScanStatusFailed;
                        view_commit_model(app->view_walk_scan, true);
                        // Restart scanning after failed connect
                        ble_walk_hal_start_scan();
                    }
                }
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        ble_walk_hal_stop_scan();
        ble_walk_hal_disconnect();
        ble_walk_hal_stop();
        consumed = false;
    } else if(event.type == SceneManagerEventTypeTick) {
        uint16_t count;
        BleWalkDevice* devices = ble_walk_hal_get_devices(&count);

        BleWalkScanModel* model = view_get_model(app->view_walk_scan);
        model->scanning = ble_walk_hal_is_scanning();
        walk_scan_publish(app, devices, count);
        model = view_get_model(app->view_walk_scan);
        // Clear failed status after a few ticks
        if(model->connect_status == WalkScanStatusFailed ||
           model->connect_status == WalkScanStatusSaved) {
            model->connect_status = WalkScanStatusNone;
        }
        view_commit_model(app->view_walk_scan, true);
    }

    return consumed;
}

void ble_spam_scene_walk_scan_on_exit(void* context) {
    UNUSED(context);
    ble_walk_hal_stop_scan();
}
