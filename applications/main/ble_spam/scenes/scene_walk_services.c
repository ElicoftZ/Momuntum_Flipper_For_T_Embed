#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../ble_uuid_db.h"

#include <esp_log.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>

#define TAG "BleWalk"
#define WALK_EXPORT_REPORT 0xFE
#define WALK_GATT_REPORT_PATH "/ext/ble/gatt_report.csv"

static void format_service_label(BleWalkUuid* uuid, char* buf, size_t buf_len) {
    const char* name = ble_uuid_db_lookup_service(uuid);
    if(name) {
        snprintf(buf, buf_len, "%s", name);
        return;
    }
    if(uuid->len == BLE_WALK_UUID_LEN_16) {
        snprintf(buf, buf_len, "0x%04X", uuid->uuid.uuid16);
    } else if(uuid->len == BLE_WALK_UUID_LEN_32) {
        snprintf(buf, buf_len, "0x%08lX", (unsigned long)uuid->uuid.uuid32);
    } else if(uuid->len == BLE_WALK_UUID_LEN_128) {
        uint8_t* u = uuid->uuid.uuid128;
        snprintf(
            buf,
            buf_len,
            "%02X%02X%02X%02X-%02X%02X",
            u[15],
            u[14],
            u[13],
            u[12],
            u[11],
            u[10]);
    } else {
        snprintf(buf, buf_len, "?");
    }
}

static void services_callback(void* context, uint32_t index) {
    BleSpamApp* app = context;
    app->walk_selected_service = index;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static bool export_gatt_report(void) {
    uint16_t service_count = 0;
    BleWalkService* services = ble_walk_hal_get_services(&service_count);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, "/ext/ble");
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(
        file, WALK_GATT_REPORT_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    static const char header[] = "service_uuid,service_start,service_end,char_uuid,char_handle,properties\n";
    if(ok) ok = storage_file_write(file, header, sizeof(header) - 1) == sizeof(header) - 1;

    for(uint16_t i = 0; ok && i < service_count; ++i) {
        char service_uuid[40];
        format_service_label(&services[i].uuid, service_uuid, sizeof(service_uuid));
        if(!ble_walk_hal_discover_chars(&services[i])) {
            ok = false;
            break;
        }
        for(uint8_t wait = 0; wait < 60 && !ble_walk_hal_chars_ready(); ++wait) {
            furi_delay_ms(50);
        }
        if(!ble_walk_hal_chars_ready()) {
            ok = false;
            break;
        }

        uint16_t char_count = 0;
        BleWalkChar* chars = ble_walk_hal_get_chars(&char_count);
        for(uint16_t c = 0; ok && c < char_count; ++c) {
            char char_uuid[40];
            char char_properties[8] = "";
            format_service_label(&chars[c].uuid, char_uuid, sizeof(char_uuid));
            if(chars[c].properties & 0x02) strlcat(char_properties, "R", sizeof(char_properties));
            if(chars[c].properties & 0x08) strlcat(char_properties, "W", sizeof(char_properties));
            if(chars[c].properties & 0x10) strlcat(char_properties, "N", sizeof(char_properties));
            char row[128];
            int length = snprintf(
                row,
                sizeof(row),
                "%s,0x%04X,0x%04X,%s,0x%04X,%s\n",
                service_uuid,
                services[i].start_handle,
                services[i].end_handle,
                char_uuid,
                chars[c].handle,
                char_properties);
            ok = length > 0 && storage_file_write(file, row, (size_t)length) == (size_t)length;
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

void ble_spam_scene_walk_services_on_enter(void* context) {
    BleSpamApp* app = context;

    ble_walk_hal_discover_services();

    // Wait for service discovery (max 3s)
    for(int i = 0; i < 60 && !ble_walk_hal_services_ready(); i++) {
        furi_delay_ms(50);
    }

    uint16_t count;
    BleWalkService* services = ble_walk_hal_get_services(&count);

    for(int i = 0; i < count; i++) {
        char label[40];
        format_service_label(&services[i].uuid, label, sizeof(label));
        submenu_add_item(app->submenu, label, i, services_callback, app);
    }

    submenu_add_item(app->submenu, "Export GATT CSV", WALK_EXPORT_REPORT, services_callback, app);

    if(count == 0) {
        submenu_add_item(app->submenu, "(no services)", 0xFF, services_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewSubmenu);
}

bool ble_spam_scene_walk_services_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WALK_EXPORT_REPORT) {
            const bool ok = export_gatt_report();
            submenu_set_header(
                app->submenu, ok ? "Saved /ext/ble/gatt_report.csv" : "GATT export failed");
            consumed = true;
        } else if(event.event != 0xFF) {
            scene_manager_next_scene(app->scene_manager, BleSpamSceneWalkChars);
            consumed = true;
        }
    }

    return consumed;
}

void ble_spam_scene_walk_services_on_exit(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);

    // If going back (not forward to chars), disconnect and stop HAL
    if(!ble_walk_hal_is_connected()) return;
    // Check if next scene is WalkChars — if not, disconnect
    // SceneManager handles this: if back event, we disconnect
}

