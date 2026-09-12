#include "assets_icons.h"
#include <gui/icon_i.h>

/* Bluetooth rune framed by scan-reticle corners on a 14x14 pixel grid. */
static const uint8_t _I_BleDetector_14_0[] = {
    0x00, 0x00, 0x00, 0x0e, 0x1c, 0x42, 0x10, 0xc2,
    0x10, 0x48, 0x01, 0x50, 0x02, 0x60, 0x01, 0xc0,
    0x00, 0x60, 0x01, 0x50, 0x02, 0x4a, 0x11, 0xc2,
    0x10, 0x4e, 0x1c, 0x00, 0x00,
};

static const uint8_t* const _I_BleDetector_14[] = {_I_BleDetector_14_0};

const Icon I_BleDetector_14 = {
    .width = 14,
    .height = 14,
    .frame_count = 1,
    .frame_rate = 0,
    .frames = _I_BleDetector_14,
};
