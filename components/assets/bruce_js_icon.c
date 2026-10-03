#include "assets_icons.h"

#include <gui/icon_i.h>

/* Bold "JS" wordmark on a 14x14 pixel grid, matching the classic JavaScript
 * logo's lettering (per user request, replacing an earlier shark-silhouette
 * design). Generated via tools/fam/compile_icons.py's png_to_icon_payload()
 * from a hand-drawn 14x14 source bitmap. */
const uint8_t _I_BruceJs_14_0[] = {
    0x00, 0xff, 0x3f, 0xe3, 0x30, 0xef, 0x3e, 0xef,
    0x38, 0xef, 0x33, 0xef, 0x33, 0xef, 0x33, 0xef,
    0x38, 0xee, 0x3e, 0xee, 0x33, 0xee, 0x33, 0xf1,
    0x33, 0x7f, 0x38, 0xff, 0x3f,
};

const uint8_t* const _I_BruceJs_14[] = {_I_BruceJs_14_0};

const Icon I_BruceJs_14 = {
    .width = 14,
    .height = 14,
    .frame_count = 1,
    .frame_rate = 0,
    .frames = _I_BruceJs_14,
};
