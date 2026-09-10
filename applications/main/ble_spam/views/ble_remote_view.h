#pragma once

#include <gui/view.h>
#include <stdbool.h>

typedef enum {
    BleRemoteModePresenter,
    BleRemoteModeMedia,
    BleRemoteModeCamera,
    BleRemoteModeMouse,
    BleRemoteModeMouseJiggler,
} BleRemoteMode;

typedef enum {
    BleRemoteEventUp = 0x600,
    BleRemoteEventDown,
    BleRemoteEventLeft,
    BleRemoteEventRight,
    BleRemoteEventOk,
} BleRemoteEvent;

typedef struct {
    BleRemoteMode mode;
    bool connected;
    bool active;
    bool no_sleep;
} BleRemoteModel;

View* ble_remote_view_alloc(void);
void ble_remote_view_free(View* view);
