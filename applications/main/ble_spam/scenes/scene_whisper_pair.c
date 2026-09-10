/* Flipper UI port of zalexdev/wpair-app Scanner.kt and VulnerabilityTester.kt.
 * Apache-2.0; upstream attribution and port differences: ../whisper_pair/NOTICE.
 * No Classic bonding, account-key persistence, or audio features. */
#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../whisper_pair_ad.h"

#include <gui/elements.h>
#include <esp_random.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    WhisperHome,
    WhisperScanning,
    WhisperList,
    WhisperDevice,
    WhisperConnecting,
    WhisperResult,
    WhisperHelp,
} WhisperState;

typedef struct WhisperPair {
    BleSpamApp* app;
    View* view;
    FuriMutex* mutex;
    FuriThread* worker;
    volatile bool abort;
    bool radio_attempted;
    bool radio_ready;
    WhisperState state;
    BleWalkDevice devices[BLE_WALK_MAX_DEVICES];
    uint16_t count;
    uint16_t selected;
    bool scan_all;
    char result[32];
} WhisperPair;

typedef struct {
    WhisperPair* instance;
} WhisperModel;

static void whisper_result(WhisperPair* wp, const char* text) {
    furi_mutex_acquire(wp->mutex, FuriWaitForever);
    snprintf(wp->result, sizeof(wp->result), "%s", text);
    wp->state = WhisperResult;
    furi_mutex_release(wp->mutex);
}

static bool whisper_fast_pair_service(const BleWalkUuid* uuid) {
    if(uuid->len == BLE_WALK_UUID_LEN_16) return uuid->uuid.uuid16 == 0xFE2C;
    if(uuid->len == BLE_WALK_UUID_LEN_32) return uuid->uuid.uuid32 == 0xFE2C;
    static const uint8_t base_uuid[16] = {
        0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x10, 0x00, 0x00, 0x2C, 0xFE, 0x00, 0x00,
    };
    return uuid->len == BLE_WALK_UUID_LEN_128 &&
           memcmp(uuid->uuid.uuid128, base_uuid, sizeof(base_uuid)) == 0;
}

static int32_t whisper_scan(void* context) {
    WhisperPair* wp = context;
    if(!wp->radio_ready) {
        wp->radio_attempted = true;
        wp->radio_ready = ble_walk_hal_start();
    }
    if(!wp->radio_ready) {
        whisper_result(wp, "Bluetooth start failed");
        return 0;
    }
    if(wp->abort) return 0;
    if(!ble_walk_hal_start_passive_scan(!wp->scan_all)) {
        whisper_result(wp, "Scan failed; try again");
        return 0;
    }
    for(unsigned i = 0; i < 80 && !wp->abort; ++i) furi_delay_ms(100);
    ble_walk_hal_stop_scan();
    if(wp->abort) return 0;

    uint16_t count = 0;
    BleWalkDevice* devices = ble_walk_hal_get_devices(&count);
    if(count > BLE_WALK_MAX_DEVICES) count = BLE_WALK_MAX_DEVICES;
    furi_mutex_acquire(wp->mutex, FuriWaitForever);
    memcpy(wp->devices, devices, count * sizeof(BleWalkDevice));
    wp->count = count;
    wp->selected = 0;
    wp->state = WhisperList;
    furi_mutex_release(wp->mutex);
    return 0;
}

static int32_t whisper_connect(void* context) {
    WhisperPair* wp = context;
    /* The list is a frozen scan snapshot; navigation is disabled during work. */
    BleWalkDevice target = wp->devices[wp->selected];
    const char* result = "Connection failed";
    char write_result[32];
    if(ble_walk_hal_connect(&target, &wp->abort) && !wp->abort) {
        result = "Service check incomplete";
        if(ble_walk_hal_discover_services()) {
            for(unsigned i = 0; i < 60 && !wp->abort &&
                                !ble_walk_hal_services_ready() &&
                                ble_walk_hal_is_connected(); ++i) {
                furi_delay_ms(50);
            }
            if(!wp->abort && ble_walk_hal_services_ready() && ble_walk_hal_is_connected()) {
                uint16_t count = 0;
                BleWalkService* services = ble_walk_hal_get_services(&count);
                /* Discovery can be truncated or fail: absence is not a clean bill of health. */
                result = "Fast Pair not observed";
                for(uint16_t i = 0; i < count; ++i) {
                    if(whisper_fast_pair_service(&services[i].uuid)) {
                        BleWalkService service = services[i];
                        result = "Characteristic unavailable";
                        if(!ble_walk_hal_discover_chars(&service)) break;
                        for(unsigned j = 0; j < 100 && !wp->abort &&
                                            !ble_walk_hal_chars_ready() &&
                                            ble_walk_hal_is_connected(); ++j) {
                            furi_delay_ms(100);
                        }
                        if(wp->abort || !ble_walk_hal_chars_ready() || !ble_walk_hal_is_connected()) break;
                        uint16_t char_count = 0;
                        BleWalkChar* chars = ble_walk_hal_get_chars(&char_count);
                        static const uint8_t key_pair_uuid[16] = {
                            0xEA, 0x0B, 0x10, 0x32, 0xDE, 0x01, 0xB0, 0x8E,
                            0x14, 0x48, 0x66, 0x83, 0x34, 0x12, 0x2C, 0xFE,
                        };
                        for(uint16_t j = 0; j < char_count; ++j) {
                            if(chars[j].uuid.len != BLE_WALK_UUID_LEN_128 ||
                               memcmp(chars[j].uuid.uuid.uuid128, key_pair_uuid, 16)) continue;
                            uint8_t request[16], salt[8];
                            esp_fill_random(salt, sizeof(salt));
                            whisper_pair_test_request(request, target.addr, salt);
                            result = "Write could not start";
                            if(wp->abort || !ble_walk_hal_write_char(chars[j].handle, request, sizeof(request))) break;
                            for(unsigned k = 0; k < 100 && !wp->abort &&
                                                !ble_walk_hal_write_ready() &&
                                                ble_walk_hal_is_connected(); ++k) {
                                furi_delay_ms(100);
                            }
                            result = "Write timeout / disconnect";
                            if(!wp->abort && ble_walk_hal_write_ready()) {
                                int status = ble_walk_hal_get_write_status();
                                if(status == 0) result = "Test write accepted";
                                else {
                                    snprintf(write_result, sizeof(write_result), "Write status: 0x%X", (unsigned)status);
                                    result = write_result;
                                }
                            }
                            break;
                        }
                        break;
                    }
                }
            }
        }
    }
    ble_walk_hal_disconnect();
    if(!wp->abort) whisper_result(wp, result);
    return 0;
}

static void whisper_join(WhisperPair* wp) {
    if(!wp->worker) return;
    furi_thread_join(wp->worker);
    furi_thread_free(wp->worker);
    wp->worker = NULL;
}

static void whisper_start(WhisperPair* wp, bool scan) {
    whisper_join(wp);
    wp->abort = false;
    furi_mutex_acquire(wp->mutex, FuriWaitForever);
    wp->state = scan ? WhisperScanning : WhisperConnecting;
    furi_mutex_release(wp->mutex);
    wp->worker = furi_thread_alloc_ex(
        "WhisperPair", 4096, scan ? whisper_scan : whisper_connect, wp);
    furi_thread_start(wp->worker);
}

static void whisper_draw(Canvas* canvas, void* model) {
    WhisperPair* wp = ((WhisperModel*)model)->instance;
    furi_mutex_acquire(wp->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "WhisperPair");
    canvas_set_font(canvas, FontSecondary);
    char text[40];
    switch(wp->state) {
    case WhisperHome:
        canvas_draw_str(canvas, 2, 23, wp->scan_all ? "Up/Down: All BLE devices" : "Up/Down: Fast Pair only");
        canvas_draw_str(canvas, 2, 34, "OK: Passive scan (8s)");
        canvas_draw_str(canvas, 2, 45, "Right: WPair / guidance");
        canvas_draw_str(canvas, 2, 61, "Back: Bluetooth menu");
        break;
    case WhisperScanning:
    case WhisperConnecting:
        canvas_draw_str(canvas, 2, 27,
                        wp->state == WhisperScanning ? "Listening for 8 seconds..." : "Connecting / checking...");
        canvas_draw_str(canvas, 2, 43, "Back: Cancel");
        break;
    case WhisperList:
        snprintf(text, sizeof(text), "%u/32 BLE; * = Fast Pair", (unsigned)wp->count);
        canvas_draw_str(canvas, 2, 21, text);
        if(!wp->count) canvas_draw_str(canvas, 2, 37, "No advertisements seen");
        for(uint16_t i = wp->selected > 1 ? wp->selected - 1 : 0, row = 0;
            i < wp->count && row < 3; ++i, ++row) {
            const BleWalkDevice* dev = &wp->devices[i];
            WhisperPairAdvertisement ad = whisper_pair_parse_ad(dev->adv_data, dev->adv_data_len);
            if(dev->name[0]) {
                snprintf(text, sizeof(text), "%c%c %.15s", i == wp->selected ? '>' : ' ',
                         ad.present ? '*' : ' ', dev->name);
            } else {
                snprintf(text, sizeof(text), "%c%c %02X:%02X:%02X:%02X:%02X:%02X",
                         i == wp->selected ? '>' : ' ', ad.present ? '*' : ' ',
                         dev->addr[0], dev->addr[1], dev->addr[2],
                         dev->addr[3], dev->addr[4], dev->addr[5]);
            }
            canvas_draw_str(canvas, 0, 32 + row * 10, text);
        }
        canvas_draw_str(canvas, 2, 63, "OK: Select  Right: Rescan");
        break;
    case WhisperDevice: {
        const BleWalkDevice* dev = &wp->devices[wp->selected];
        WhisperPairAdvertisement ad = whisper_pair_parse_ad(dev->adv_data, dev->adv_data_len);
        snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X %d",
                 dev->addr[0], dev->addr[1], dev->addr[2],
                 dev->addr[3], dev->addr[4], dev->addr[5], dev->rssi);
        canvas_draw_str(canvas, 2, 22, text);
        if(ad.has_model) snprintf(text, sizeof(text), "Model ID: %06lX", (unsigned long)ad.model);
        else snprintf(text, sizeof(text), "%s", ad.present ? "Fast Pair advertised" : "Fast Pair not advertised");
        canvas_draw_str(canvas, 2, 32, text);
        canvas_draw_str(canvas, 2, 42, ad.pairing_mode ? "Exit pairing mode; rescan" : "Own device; pairing OFF");
        canvas_draw_str(canvas, 2, 52, ad.pairing_mode ? "Test disabled in this mode" : "OK: Connect + WPair test");
        canvas_draw_str(canvas, 2, 63, "Back: Device list");
        break;
    }
    case WhisperResult:
        canvas_draw_str(canvas, 2, 23, wp->result);
        canvas_draw_str(canvas, 2, 34, "CVE status: inconclusive");
        canvas_draw_str(canvas, 2, 45, "BLE test; no Classic bond");
        canvas_draw_str(canvas, 2, 61, "Back: List  Right: Guidance");
        break;
    case WhisperHelp:
        canvas_draw_str(canvas, 2, 23, "Port: zalexdev/wpair-app");
        canvas_draw_str(canvas, 2, 33, "S3: no Classic / HFP.");
        canvas_draw_str(canvas, 2, 43, "Keep firmware updated.");
        canvas_draw_str(canvas, 2, 53, "Results are inconclusive.");
        canvas_draw_str(canvas, 2, 63, "Back: Home");
        break;
    }
    furi_mutex_release(wp->mutex);
}

static bool whisper_input(InputEvent* event, void* context) {
    WhisperPair* wp = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return true;
    if(event->type == InputTypeRepeat && event->key != InputKeyUp && event->key != InputKeyDown) return true;
    view_dispatcher_send_custom_event(wp->app->view_dispatcher, event->key);
    return true;
}

void ble_spam_scene_whisper_pair_on_enter(void* context) {
    BleSpamApp* app = context;
    WhisperPair* wp = calloc(1, sizeof(WhisperPair));
    furi_check(wp);
    app->whisper_pair = wp;
    wp->app = app;
    wp->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    wp->view = view_alloc();
    view_allocate_model(wp->view, ViewModelTypeLockFree, sizeof(WhisperModel));
    ((WhisperModel*)view_get_model(wp->view))->instance = wp;
    view_commit_model(wp->view, false);
    view_set_context(wp->view, wp);
    view_set_draw_callback(wp->view, whisper_draw);
    view_set_input_callback(wp->view, whisper_input);
    view_dispatcher_add_view(app->view_dispatcher, BleSpamViewWhisperPair, wp->view);
    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewWhisperPair);
}

bool ble_spam_scene_whisper_pair_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    WhisperPair* wp = app->whisper_pair;
    if(event.type == SceneManagerEventTypeTick) {
        view_commit_model(wp->view, true);
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom && event.type != SceneManagerEventTypeBack) return false;
    InputKey key = event.type == SceneManagerEventTypeBack ? InputKeyBack : (InputKey)event.event;
    furi_mutex_acquire(wp->mutex, FuriWaitForever);
    WhisperState state = wp->state;
    furi_mutex_release(wp->mutex);
    if(key == InputKeyBack && state == WhisperHome) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    if(state == WhisperScanning || state == WhisperConnecting) {
        if(key != InputKeyBack) return true;
        wp->abort = true;
        whisper_join(wp);
    } else if((state == WhisperHome && key == InputKeyOk) ||
              (state == WhisperList && key == InputKeyRight)) {
        whisper_start(wp, true);
        return true;
    } else if(state == WhisperDevice && key == InputKeyOk) {
        const BleWalkDevice* dev = &wp->devices[wp->selected];
        WhisperPairAdvertisement ad = whisper_pair_parse_ad(dev->adv_data, dev->adv_data_len);
        if(!ad.pairing_mode) whisper_start(wp, false);
        return true;
    }
    furi_mutex_acquire(wp->mutex, FuriWaitForever);
    if(key == InputKeyBack) {
        wp->state = (state == WhisperDevice || state == WhisperResult || state == WhisperConnecting) ?
                        WhisperList : WhisperHome;
    } else if(key == InputKeyRight && (state == WhisperHome || state == WhisperResult)) {
        wp->state = WhisperHelp;
    } else if(state == WhisperHome && (key == InputKeyUp || key == InputKeyDown)) {
        wp->scan_all = !wp->scan_all;
    } else if(state == WhisperList && wp->count) {
        if(key == InputKeyUp && wp->selected) wp->selected--;
        if(key == InputKeyDown && wp->selected + 1 < wp->count) wp->selected++;
        if(key == InputKeyOk) wp->state = WhisperDevice;
    }
    furi_mutex_release(wp->mutex);
    view_commit_model(wp->view, true);
    return true;
}

void ble_spam_scene_whisper_pair_on_exit(void* context) {
    BleSpamApp* app = context;
    WhisperPair* wp = app->whisper_pair;
    wp->abort = true;
    whisper_join(wp);
    if(wp->radio_attempted) ble_walk_hal_stop();
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewWhisperPair);
    view_free(wp->view);
    furi_mutex_free(wp->mutex);
    free(wp);
    app->whisper_pair = NULL;
}
