#pragma once

/* Minimal CBOR (RFC 8949) reader/writer for CTAP2.
 *
 * CTAP2 uses a deliberately small subset -- "CTAP2 canonical CBOR" -- so this
 * is not a general CBOR library and does not try to be:
 *
 *  - Definite lengths only. Indefinite-length items are rejected on read and
 *    cannot be written; CTAP2 canonical form forbids them.
 *  - Shortest-form argument encoding, which the writer produces naturally.
 *  - No tags, no floats, no indefinite chunks, no half/single/double reads.
 *
 * Map key ordering is the caller's job: every map this firmware emits has a
 * fixed, known key set, so the encoders below simply write them already sorted
 * rather than buffering and sorting at runtime.
 *
 * The reader is zero-copy -- bstr/tstr hand back pointers into the buffer the
 * caller supplied, so that buffer must outlive every pointer taken from it.
 *
 * Deliberately free of furi and ESP-IDF dependencies so it can be built and
 * tested with MSVC on the host (tests/host/fido_cbor_test.c).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CBOR major types, in the wire encoding's own numbering. */
typedef enum {
    CborTypeUint = 0,
    CborTypeNegint = 1,
    CborTypeBstr = 2,
    CborTypeTstr = 3,
    CborTypeArray = 4,
    CborTypeMap = 5,
    CborTypeTag = 6,
    CborTypeSimple = 7,
    CborTypeInvalid = 0xFF,
} CborType;

/* ------------------------------------------------------------------ writer */

typedef struct {
    uint8_t* buf;
    size_t cap;
    size_t len;
    bool error; /* sticky: set on overflow, never cleared */
} CborWriter;

void cbor_w_init(CborWriter* w, uint8_t* buf, size_t cap);

/* Signed helper picking major type 0 or 1. CTAP2 needs negative values for the
 * COSE labels (-1..-3) and algorithm identifiers (-7), so callers should reach
 * for this rather than the two majors directly. */
void cbor_w_int(CborWriter* w, int64_t value);
void cbor_w_uint(CborWriter* w, uint64_t value);

void cbor_w_bstr(CborWriter* w, const void* data, size_t len);
void cbor_w_tstr_n(CborWriter* w, const char* data, size_t len);
void cbor_w_tstr(CborWriter* w, const char* str); /* strlen() */
void cbor_w_bool(CborWriter* w, bool value);
void cbor_w_null(CborWriter* w);

/* Headers only -- the caller then writes `count` values, or `count` key/value
 * PAIRS for a map. Nothing checks that it does. */
void cbor_w_map(CborWriter* w, size_t count);
void cbor_w_array(CborWriter* w, size_t count);

/* Splice already-encoded CBOR in verbatim. */
void cbor_w_raw(CborWriter* w, const void* data, size_t len);

/* Reserve space for a bstr of `len` bytes and hand back a pointer to write
 * into, so large payloads (authData) can be built in place instead of in a
 * scratch buffer and copied. Returns NULL if it would not fit. */
uint8_t* cbor_w_bstr_reserve(CborWriter* w, size_t len);

/** Total encoded length, or 0 if anything overflowed. Never returns a
 * truncated buffer's length -- a partial CBOR message is worse than none. */
size_t cbor_w_finish(const CborWriter* w);

/* ------------------------------------------------------------------ reader */

typedef struct {
    const uint8_t* buf;
    size_t len;
    size_t pos;
    bool error; /* sticky */
} CborReader;

void cbor_r_init(CborReader* r, const void* buf, size_t len);

/** Major type of the next item without consuming it.
 * CborTypeInvalid at end of buffer, on a malformed head, or once errored. */
CborType cbor_r_peek(const CborReader* r);

bool cbor_r_uint(CborReader* r, uint64_t* out);
bool cbor_r_int(CborReader* r, int64_t* out); /* accepts major 0 and 1 */
bool cbor_r_bool(CborReader* r, bool* out);

/* Zero-copy: *data points into the reader's buffer. */
bool cbor_r_bstr(CborReader* r, const uint8_t** data, size_t* len);
bool cbor_r_tstr(CborReader* r, const char** data, size_t* len);

/* Consume the header and report the element count. The caller then reads that
 * many items (twice that, for a map). */
bool cbor_r_map(CborReader* r, size_t* count);
bool cbor_r_array(CborReader* r, size_t* count);

/** Skip one complete item, nested containers and all. Required: CTAP2 clients
 * routinely send map entries and extensions this firmware does not implement,
 * and the parser has to step over them rather than give up. */
bool cbor_r_skip(CborReader* r);

/** True when every byte has been consumed and nothing has errored. */
bool cbor_r_done(const CborReader* r);

/* Convenience for the dominant CTAP2 shape: a text string compared against a
 * literal. Avoids a strncmp plus a length check at every call site. */
bool cbor_r_tstr_equals(const char* data, size_t len, const char* literal);

#ifdef __cplusplus
}
#endif
