#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <storage/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Decode the source PNG format used by Momentum asset packs.
 *
 * Only 1-bit greyscale, non-interlaced PNGs are accepted. On success, bitmap
 * is an allocated Flipper icon frame (the first byte is the uncompressed 0x00
 * marker) and must be freed by the caller.
 */
bool momentum_png_icon_load(
    File* file,
    const char* path,
    uint8_t** bitmap,
    size_t* bitmap_size,
    uint32_t* width,
    uint32_t* height);

#ifdef __cplusplus
}
#endif
