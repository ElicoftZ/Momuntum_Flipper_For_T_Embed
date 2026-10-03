#pragma once

#include <gui/view.h>
#include "../ble_walk_hal.h"

#define WALK_SCAN_ITEMS_ON_SCREEN 4

typedef enum {
    WalkScanStatusNone,
    WalkScanStatusConnecting,
    WalkScanStatusConnected,
    WalkScanStatusFailed,
    WalkScanStatusSaved,
} WalkScanStatus;

typedef struct {
    BleWalkDevice devices[BLE_WALK_MAX_DEVICES];
    uint8_t source_indices[BLE_WALK_MAX_DEVICES];
    bool watchlisted[BLE_WALK_MAX_DEVICES];
    uint16_t count;
    uint16_t selected;
    uint16_t window_offset;
    uint8_t filter;
    bool scanning;
    WalkScanStatus connect_status;
} BleWalkScanModel;

View* ble_walk_scan_view_alloc(void);
void ble_walk_scan_view_free(View* view);
