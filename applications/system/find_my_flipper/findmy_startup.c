#include "findmy_state.h"
#include <furi.h>
#include <furi_hal.h>
#include <bt/bt_service/bt.h>
#include <storage/storage.h>

#define TAG "FindMyStartup"

static int32_t findmy_startup_apply(void* context) {
    UNUSED(context);
    FURI_LOG_D(TAG, "Loading state");

    // Wait for BT init and check core2
    furi_record_open(RECORD_BT);
    furi_record_close(RECORD_BT);
    if(!furi_hal_bt_is_gatt_gap_supported()) return 0;

    FindMyState state;
    if(findmy_state_load(&state)) {
        FURI_LOG_D(TAG, "Activating beacon");
        findmy_state_apply(&state);
    } else {
        FURI_LOG_D(TAG, "Beacon not active, bailing");
    }

    return 0;
}

static void findmy_startup_thread_state_callback(
    FuriThread* thread,
    FuriThreadState state,
    void* context) {
    UNUSED(context);

    if(state == FuriThreadStateStopped) {
        furi_thread_free(thread);
    }
}

static void findmy_startup_apply_async(void) {
    FuriThread* thread =
        furi_thread_alloc_ex("FindMyStartup", 2048, findmy_startup_apply, NULL);
    furi_thread_set_state_callback(thread, findmy_startup_thread_state_callback);
    furi_thread_start(thread);
}

static void findmy_startup_mount_callback(const void* message, void* context) {
    UNUSED(context);
    const StorageEvent* event = message;

    if(event->type == StorageEventTypeCardMount) {
        findmy_startup_apply_async();
    }
}

void findmy_startup() {
    if(furi_hal_rtc_get_boot_mode() != FuriHalRtcBootModeNormal) return;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    furi_pubsub_subscribe(storage_get_pubsub(storage), findmy_startup_mount_callback, NULL);

    if(storage_sd_status(storage) != FSE_OK) {
        FURI_LOG_D(TAG, "SD Card not ready, skipping startup hook");
    } else {
        findmy_startup_apply(NULL);
    }

    furi_record_close(RECORD_STORAGE);
}
