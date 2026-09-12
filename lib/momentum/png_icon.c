#include "png_icon.h"

#include <furi.h>
#include <miniz.h>

#include <esp_heap_caps.h>

#include <stdlib.h>
#include <string.h>

#define PNG_MAX_FILE_SIZE (256U * 1024U)
#define PNG_MAX_DIMENSION 1024U

static const uint8_t png_signature[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

static void* png_alloc(size_t size) {
    void* buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return buffer ? buffer : malloc(size);
}

typedef struct {
    uint8_t* out;
    size_t capacity;
    size_t written;
} PngInflateSink;

/* tinfl flusht seinen internen 32-KB-Puffer hier durch; 0 bricht ab. Der
 * Callback-Weg ist Absicht: tinfl_decompress_mem_to_mem() setzt intern
 * TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF und loest LZ77-Rueckwaertsreferenzen
 * direkt im Zielpuffer auf -- bei einem ~1 KB grossen Icon-Puffer liest eine
 * Distanz, die ueber das bisher Dekodierte hinausgeht, VOR den Pufferanfang.
 * Diese Variante nutzt stattdessen das interne 32-KB-Woerterbuch. */
static int png_put_buf(const void* buf, int len, void* user) {
    PngInflateSink* sink = user;
    if(len <= 0) return 1;
    if((size_t)len > sink->capacity - sink->written) return 0;
    memcpy(sink->out + sink->written, buf, (size_t)len);
    sink->written += (size_t)len;
    return 1;
}

static uint32_t png_read_be32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static uint8_t png_paeth(uint8_t left, uint8_t above, uint8_t upper_left) {
    const int p = (int)left + (int)above - (int)upper_left;
    const int pa = abs(p - (int)left);
    const int pb = abs(p - (int)above);
    const int pc = abs(p - (int)upper_left);
    if(pa <= pb && pa <= pc) return left;
    return pb <= pc ? above : upper_left;
}

static uint8_t png_reverse_bits(uint8_t value) {
    value = (uint8_t)(((value & 0x55U) << 1) | ((value & 0xAAU) >> 1));
    value = (uint8_t)(((value & 0x33U) << 2) | ((value & 0xCCU) >> 2));
    return (uint8_t)((value << 4) | (value >> 4));
}

static bool png_unfilter(uint8_t* raw, uint32_t height, size_t row_bytes) {
    const size_t stride = row_bytes + 1U;

    for(uint32_t row = 0; row < height; row++) {
        uint8_t* const line = raw + ((size_t)row * stride) + 1U;
        const uint8_t* const previous = row ? line - stride : NULL;
        const uint8_t filter = line[-1];

        if(filter > 4U) return false;
        for(size_t column = 0; column < row_bytes; column++) {
            const uint8_t left = column ? line[column - 1U] : 0U;
            const uint8_t above = previous ? previous[column] : 0U;
            const uint8_t upper_left = previous && column ? previous[column - 1U] : 0U;

            switch(filter) {
            case 0:
                break;
            case 1:
                line[column] = (uint8_t)(line[column] + left);
                break;
            case 2:
                line[column] = (uint8_t)(line[column] + above);
                break;
            case 3:
                line[column] = (uint8_t)(line[column] + ((left + above) >> 1));
                break;
            case 4:
                line[column] =
                    (uint8_t)(line[column] + png_paeth(left, above, upper_left));
                break;
            }
        }
    }

    return true;
}

bool momentum_png_icon_load(
    File* file,
    const char* path,
    uint8_t** bitmap,
    size_t* bitmap_size,
    uint32_t* width,
    uint32_t* height) {
    furi_assert(file);
    furi_assert(path);
    furi_assert(bitmap);
    furi_assert(bitmap_size);
    furi_assert(width);
    furi_assert(height);

    *bitmap = NULL;
    *bitmap_size = 0;
    *width = 0;
    *height = 0;

    uint8_t* png = NULL;
    uint8_t* idat = NULL;
    uint8_t* raw = NULL;
    uint8_t* result = NULL;
    bool ok = false;

    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) goto done;
    const uint64_t file_size_64 = storage_file_size(file);
    if(file_size_64 < sizeof(png_signature) + 12U || file_size_64 > PNG_MAX_FILE_SIZE) goto done;

    const size_t file_size = (size_t)file_size_64;
    png = png_alloc(file_size);
    if(!png || storage_file_read(file, png, file_size) != file_size) goto done;
    if(memcmp(png, png_signature, sizeof(png_signature))) goto done;

    size_t offset = sizeof(png_signature);
    size_t idat_size = 0;
    bool have_header = false;
    uint32_t parsed_width = 0;
    uint32_t parsed_height = 0;

    while(offset + 12U <= file_size) {
        const uint32_t chunk_size = png_read_be32(png + offset);
        const size_t payload = offset + 8U;
        if((size_t)chunk_size > file_size - payload - 4U) goto done;
        const uint8_t* const type = png + offset + 4U;

        if(!memcmp(type, "IHDR", 4)) {
            if(chunk_size != 13U || have_header) goto done;
            parsed_width = png_read_be32(png + payload);
            parsed_height = png_read_be32(png + payload + 4U);
            if(!parsed_width || !parsed_height || parsed_width > PNG_MAX_DIMENSION ||
               parsed_height > PNG_MAX_DIMENSION || png[payload + 8U] != 1U ||
               png[payload + 9U] != 0U || png[payload + 10U] != 0U ||
               png[payload + 11U] != 0U || png[payload + 12U] != 0U) {
                goto done;
            }
            have_header = true;
        } else if(!memcmp(type, "IDAT", 4)) {
            if(idat_size > file_size - chunk_size) goto done;
            idat_size += chunk_size;
        } else if(!memcmp(type, "IEND", 4)) {
            break;
        }

        offset = payload + chunk_size + 4U;
    }

    if(!have_header || !idat_size) goto done;
    idat = png_alloc(idat_size);
    if(!idat) goto done;

    offset = sizeof(png_signature);
    size_t idat_offset = 0;
    while(offset + 12U <= file_size) {
        const uint32_t chunk_size = png_read_be32(png + offset);
        const size_t payload = offset + 8U;
        if((size_t)chunk_size > file_size - payload - 4U) goto done;
        const uint8_t* const type = png + offset + 4U;
        if(!memcmp(type, "IDAT", 4)) {
            memcpy(idat + idat_offset, png + payload, chunk_size);
            idat_offset += chunk_size;
        } else if(!memcmp(type, "IEND", 4)) {
            break;
        }
        offset = payload + chunk_size + 4U;
    }
    if(idat_offset != idat_size) goto done;

    const size_t row_bytes = ((size_t)parsed_width + 7U) / 8U;
    if(row_bytes > (SIZE_MAX / parsed_height) - 1U) goto done;
    const size_t raw_size = (row_bytes + 1U) * parsed_height;
    raw = png_alloc(raw_size);
    if(!raw) goto done;
    PngInflateSink sink = {.out = raw, .capacity = raw_size, .written = 0};
    size_t idat_consumed = idat_size;
    if(!tinfl_decompress_mem_to_callback(
           idat, &idat_consumed, png_put_buf, &sink, TINFL_FLAG_PARSE_ZLIB_HEADER) ||
       sink.written != raw_size) {
        goto done;
    }
    if(!png_unfilter(raw, parsed_height, row_bytes)) goto done;

    const size_t result_size = 1U + row_bytes * parsed_height;
    result = png_alloc(result_size);
    if(!result) goto done;
    result[0] = 0U;

    const uint8_t tail_bits = (uint8_t)(parsed_width & 7U);
    const uint8_t tail_mask = tail_bits ? (uint8_t)((1U << tail_bits) - 1U) : 0xFFU;
    const size_t stride = row_bytes + 1U;
    for(uint32_t row = 0; row < parsed_height; row++) {
        const uint8_t* const source = raw + ((size_t)row * stride) + 1U;
        uint8_t* const target = result + 1U + ((size_t)row * row_bytes);
        for(size_t column = 0; column < row_bytes; column++) {
            uint8_t value = png_reverse_bits((uint8_t)~source[column]);
            if(column == row_bytes - 1U) value &= tail_mask;
            target[column] = value;
        }
    }

    *bitmap = result;
    *bitmap_size = result_size;
    *width = parsed_width;
    *height = parsed_height;
    result = NULL;
    ok = true;

done:
    storage_file_close(file);
    free(result);
    free(raw);
    free(idat);
    free(png);
    return ok;
}
