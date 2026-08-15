#pragma once

#include <gui/canvas.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASSET_PACKS_PATH EXT_PATH("asset_packs")

/** Load the configured pack's icons and fonts. No-op if already loaded or if
 * no pack is selected. */
void asset_packs_init(void);

/** Release everything asset_packs_init() loaded. */
void asset_packs_free(void);

/** Replacement for a compiled-in icon, or the icon itself when no pack
 * provides one. */
const Icon* asset_packs_swap_icon(const Icon* requested);

/** Replacement u8g2 font data for a font, or NULL to use the compiled one. */
const uint8_t* asset_packs_swap_font(Font font);

/** Replacement metrics for a font, or NULL to use the compiled ones. */
const CanvasFontParameters* asset_packs_swap_font_params(Font font);

#ifdef __cplusplus
}
#endif
