/**
 * @file furi_hal_display.h
 * Display HAL API (ESP32-C6, ST7789V2 via esp_lcd)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_lcd_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize display hardware (ST7789V2 via SPI + esp_lcd)
 */
void furi_hal_display_init(void);

/** Commit display buffer to screen
 *
 * Converts u8g2 mono framebuffer (128x64, 1bpp tile format) to
 * RGB565 with aspect-fit scaling and sends it to ST7789V2 via DMA.
 *
 * @param      data  pointer to u8g2 framebuffer data
 * @param      size  size of framebuffer data in bytes
 */
void furi_hal_display_commit(const uint8_t* data, uint32_t size);

/** Set display backlight brightness
 *
 * @param      brightness  brightness level [0-255]
 */
void furi_hal_display_set_backlight(uint8_t brightness);

/** Put the display panel into sleep mode (ST7789 SLPIN + display off).
 *
 * Intended for the power-off / deep-sleep path: the panel keeps drawing
 * ~10-15 mA while idle otherwise. Safe to call without holding the SPI bus
 * lock — it grabs the lock internally.
 */
void furi_hal_display_sleep(void);

/** Set the UI foreground color (the tint that fills the "ink" of every
 * monochrome u8g2 frame on this color port). Stored as RGB565, byte-swapped
 * for the ST7789 SPI byte order.
 *
 * Takes effect on the next frame commit (no full redraw needed).
 *
 * @param      color  RGB565 (byte-swapped) color value
 */
void furi_hal_display_set_fg_color(uint16_t color);

/** Get the current UI foreground color (RGB565 byte-swapped). Useful for
 * spectrum/animation drivers that need to derive the next frame's color.
 */
uint16_t furi_hal_display_get_fg_color(void);

/** Set/get the UI background color — fills the "set" mono pixels (the drawn
 * UI elements). Default is BOARD_LCD_BG_COLOR (typically black). Pairs with
 * fg_color to give the user full control of the bichromatic UI palette. */
void furi_hal_display_set_bg_color(uint16_t color);
uint16_t furi_hal_display_get_bg_color(void);

/** Maximum gradient stops the UI Background can blend between. */
#define FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS 4

/** How the gradient stops are mixed down the panel. */
typedef enum {
    FuriHalDisplayGradientLinear = 0, /**< even blend between stops */
    FuriHalDisplayGradientSmooth,     /**< eased blend, lingers near each stop */
    FuriHalDisplayGradientBands,      /**< hard steps, no blending */
    FuriHalDisplayGradientMirror,     /**< down to the middle and back */
    FuriHalDisplayGradientModeCount,
} FuriHalDisplayGradientMode;

/** Replace the flat UI Background with a vertical gradient.
 *
 * Builds a per-row lookup table spanning the whole panel height, so the
 * centering margins share the ramp and rendering stays one lookup per row.
 * Passing count < 2 (or a NULL array) turns the gradient off and returns the
 * display to the flat fg_color.
 *
 * @param      stops_rgb888  Array of 0xRRGGBB stops, top to bottom
 * @param      count         Number of stops, 2..FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS
 * @param      mode          A FuriHalDisplayGradientMode value
 * @param      horizontal    true = ramp across the panel, false = down it
 */
void furi_hal_display_set_bg_gradient(
    const uint32_t* stops_rgb888,
    uint8_t count,
    uint8_t mode,
    bool horizontal);

/** Get native panel dimensions (post swap_xy). Intended for full-screen
 * takeover apps (e.g. game emulators) that bypass the 128x64 framebuffer.
 */
uint16_t furi_hal_display_get_h_res(void);
uint16_t furi_hal_display_get_v_res(void);

/** Get the underlying esp_lcd panel handle for direct full-screen drawing.
 *
 * The returned handle must only be used while the caller holds the SPI bus
 * lock (furi_hal_spi_bus_lock) and has acquired fullscreen access via the
 * GUI service (gui_direct_draw_acquire). The Furi GUI HAL keeps rendering
 * its 128x64 framebuffer, so callers that do not pause the GUI will flicker.
 */
esp_lcd_panel_handle_t furi_hal_display_get_panel_handle(void);

#ifdef __cplusplus
}
#endif
