#include "ble_remote_view.h"

#include <gui/canvas.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static const char* ble_remote_mode_name(BleRemoteMode mode) {
    switch(mode) {
    case BleRemoteModePresenter:
        return "Presenter";
    case BleRemoteModeMedia:
        return "Media Remote";
    case BleRemoteModeCamera:
        return "Camera Shutter";
    case BleRemoteModeMouse:
        return "Mouse";
    case BleRemoteModeMouseJiggler:
        return "Mouse Jiggler";
    default:
        return "BLE Remote";
    }
}

static void ble_remote_draw_callback(Canvas* canvas, void* context) {
    BleRemoteModel* model = context;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, ble_remote_mode_name(model->mode));
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);
    switch(model->mode) {
    case BleRemoteModePresenter:
        canvas_draw_str(canvas, 2, 24, "Turn: Previous / Next");
        canvas_draw_str(canvas, 2, 35, "Hold+turn: Left / Right");
        canvas_draw_str(canvas, 2, 46, "OK: Advance slide");
        break;
    case BleRemoteModeMedia:
        canvas_draw_str(canvas, 2, 24, "Turn: Previous / Next");
        canvas_draw_str(canvas, 2, 35, "Hold+turn: Volume - / +");
        canvas_draw_str(canvas, 2, 46, "OK: Play / Pause");
        break;
    case BleRemoteModeCamera:
        canvas_draw_str(canvas, 2, 28, "Open the phone camera");
        canvas_draw_str(canvas, 2, 42, "OK: Take photo");
        break;
    case BleRemoteModeMouse:
        canvas_draw_str(canvas, 2, 24, "Turn: Up / Down");
        canvas_draw_str(canvas, 2, 35, "Hold+turn: Left / Right");
        canvas_draw_str(canvas, 2, 46, "OK: Left click");
        break;
    case BleRemoteModeMouseJiggler:
        canvas_draw_str(canvas, 2, 27, "OK: Start / Stop");
        canvas_draw_str(canvas, 2, 41, "Moves every 10 seconds");
        break;
    }

    canvas_set_font(canvas, FontPrimary);
    const char* status;
    if(model->no_sleep) {
        status = model->connected ? "Connected | No sleep" : "Pair remote | No sleep";
    } else if(model->mode == BleRemoteModeMouseJiggler && model->connected && model->active) {
        status = "Connected | Running";
    } else {
        status = model->connected ? "Connected" : "Pair: Momentum BLE Remote";
    }
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, status);
}

static bool ble_remote_input_callback(InputEvent* event, void* context) {
    if(event->type != InputTypeShort) {
        return false;
    }

    ViewDispatcher* view_dispatcher = context;
    uint32_t custom_event;
    switch(event->key) {
    case InputKeyUp:
        custom_event = BleRemoteEventUp;
        break;
    case InputKeyDown:
        custom_event = BleRemoteEventDown;
        break;
    case InputKeyLeft:
        custom_event = BleRemoteEventLeft;
        break;
    case InputKeyRight:
        custom_event = BleRemoteEventRight;
        break;
    case InputKeyOk:
        custom_event = BleRemoteEventOk;
        break;
    default:
        return false;
    }

    view_dispatcher_send_custom_event(view_dispatcher, custom_event);
    return true;
}

View* ble_remote_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(BleRemoteModel));
    view_set_draw_callback(view, ble_remote_draw_callback);
    view_set_input_callback(view, ble_remote_input_callback);
    return view;
}

void ble_remote_view_free(View* view) {
    view_free(view);
}
