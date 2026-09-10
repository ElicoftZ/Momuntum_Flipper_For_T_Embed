#include "fido_cbor.h"

#include <string.h>

/* Major type occupies the top 3 bits; the low 5 are the "additional info",
 * which either IS the argument (0..23) or says how many bytes follow. */
#define CBOR_AI_MASK  0x1Fu
#define CBOR_AI_1BYTE 24u
#define CBOR_AI_2BYTE 25u
#define CBOR_AI_4BYTE 26u
#define CBOR_AI_8BYTE 27u
/* 28..30 are reserved and 31 marks an indefinite length / break. CTAP2
 * canonical CBOR forbids all of them, so they are read as malformed. */
#define CBOR_AI_MIN_RESERVED 28u

#define CBOR_SIMPLE_FALSE 20u
#define CBOR_SIMPLE_TRUE  21u
#define CBOR_SIMPLE_NULL  22u

/* A malformed message must not be able to spin the skipper forever, and a
 * legitimate CTAP2 message nests only a handful of levels deep. */
#define CBOR_SKIP_MAX_ITEMS 4096u

/* ------------------------------------------------------------------ writer */

void cbor_w_init(CborWriter* w, uint8_t* buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->error = (buf == NULL);
}

/* Every write funnels through here, so the overflow check lives in one place
 * and a writer that has already overflowed stays poisoned. */
static bool cbor_w_room(CborWriter* w, size_t need) {
    if(w->error) return false;
    if(need > w->cap - w->len) {
        w->error = true;
        return false;
    }
    return true;
}

static void cbor_w_byte(CborWriter* w, uint8_t b) {
    if(!cbor_w_room(w, 1)) return;
    w->buf[w->len++] = b;
}

/* Shortest-form argument encoding, which is what canonical CBOR requires. */
static void cbor_w_head(CborWriter* w, uint8_t major, uint64_t arg) {
    uint8_t hi = (uint8_t)(major << 5);

    if(arg < CBOR_AI_1BYTE) {
        cbor_w_byte(w, (uint8_t)(hi | arg));
    } else if(arg <= 0xFFu) {
        cbor_w_byte(w, (uint8_t)(hi | CBOR_AI_1BYTE));
        cbor_w_byte(w, (uint8_t)arg);
    } else if(arg <= 0xFFFFu) {
        cbor_w_byte(w, (uint8_t)(hi | CBOR_AI_2BYTE));
        cbor_w_byte(w, (uint8_t)(arg >> 8));
        cbor_w_byte(w, (uint8_t)arg);
    } else if(arg <= 0xFFFFFFFFu) {
        cbor_w_byte(w, (uint8_t)(hi | CBOR_AI_4BYTE));
        cbor_w_byte(w, (uint8_t)(arg >> 24));
        cbor_w_byte(w, (uint8_t)(arg >> 16));
        cbor_w_byte(w, (uint8_t)(arg >> 8));
        cbor_w_byte(w, (uint8_t)arg);
    } else {
        cbor_w_byte(w, (uint8_t)(hi | CBOR_AI_8BYTE));
        for(int shift = 56; shift >= 0; shift -= 8) {
            cbor_w_byte(w, (uint8_t)(arg >> shift));
        }
    }
}

void cbor_w_uint(CborWriter* w, uint64_t value) {
    cbor_w_head(w, CborTypeUint, value);
}

void cbor_w_int(CborWriter* w, int64_t value) {
    if(value >= 0) {
        cbor_w_head(w, CborTypeUint, (uint64_t)value);
    } else {
        /* Major 1 encodes -1-arg. Forming the argument as -(value + 1) rather
         * than -value keeps INT64_MIN in range: -(INT64_MIN + 1) is INT64_MAX. */
        cbor_w_head(w, CborTypeNegint, (uint64_t)(-(value + 1)));
    }
}

void cbor_w_bstr(CborWriter* w, const void* data, size_t len) {
    cbor_w_head(w, CborTypeBstr, len);
    if(!cbor_w_room(w, len)) return;
    if(len > 0) memcpy(w->buf + w->len, data, len);
    w->len += len;
}

uint8_t* cbor_w_bstr_reserve(CborWriter* w, size_t len) {
    cbor_w_head(w, CborTypeBstr, len);
    if(!cbor_w_room(w, len)) return NULL;
    uint8_t* out = w->buf + w->len;
    w->len += len;
    return out;
}

void cbor_w_tstr_n(CborWriter* w, const char* data, size_t len) {
    cbor_w_head(w, CborTypeTstr, len);
    if(!cbor_w_room(w, len)) return;
    if(len > 0) memcpy(w->buf + w->len, data, len);
    w->len += len;
}

void cbor_w_tstr(CborWriter* w, const char* str) {
    cbor_w_tstr_n(w, str, strlen(str));
}

void cbor_w_bool(CborWriter* w, bool value) {
    cbor_w_byte(
        w, (uint8_t)((CborTypeSimple << 5) | (value ? CBOR_SIMPLE_TRUE : CBOR_SIMPLE_FALSE)));
}

void cbor_w_null(CborWriter* w) {
    cbor_w_byte(w, (uint8_t)((CborTypeSimple << 5) | CBOR_SIMPLE_NULL));
}

void cbor_w_map(CborWriter* w, size_t count) {
    cbor_w_head(w, CborTypeMap, count);
}

void cbor_w_array(CborWriter* w, size_t count) {
    cbor_w_head(w, CborTypeArray, count);
}

void cbor_w_raw(CborWriter* w, const void* data, size_t len) {
    if(!cbor_w_room(w, len)) return;
    if(len > 0) memcpy(w->buf + w->len, data, len);
    w->len += len;
}

size_t cbor_w_finish(const CborWriter* w) {
    return w->error ? 0 : w->len;
}

/* ------------------------------------------------------------------ reader */

void cbor_r_init(CborReader* r, const void* buf, size_t len) {
    r->buf = (const uint8_t*)buf;
    r->len = len;
    r->pos = 0;
    r->error = (buf == NULL && len > 0);
}

/* Consume one item head. Poisons the reader on anything malformed, so callers
 * can chain reads and check once at the end. */
static bool cbor_r_head(CborReader* r, uint8_t* major, uint64_t* arg) {
    if(r->error || r->pos >= r->len) {
        r->error = true;
        return false;
    }

    uint8_t initial = r->buf[r->pos++];
    uint8_t ai = initial & CBOR_AI_MASK;
    *major = (uint8_t)(initial >> 5);

    if(ai < CBOR_AI_1BYTE) {
        *arg = ai;
        return true;
    }
    if(ai >= CBOR_AI_MIN_RESERVED) {
        /* Reserved, or an indefinite length / break that canonical CBOR bans. */
        r->error = true;
        return false;
    }

    size_t width = (size_t)1u << (ai - CBOR_AI_1BYTE); /* 24->1, 25->2, 26->4, 27->8 */
    if(width > r->len - r->pos) {
        r->error = true;
        return false;
    }

    uint64_t value = 0;
    for(size_t i = 0; i < width; i++) {
        value = (value << 8) | r->buf[r->pos++];
    }
    *arg = value;
    return true;
}

CborType cbor_r_peek(const CborReader* r) {
    if(r->error || r->pos >= r->len) return CborTypeInvalid;

    uint8_t initial = r->buf[r->pos];
    if((initial & CBOR_AI_MASK) >= CBOR_AI_MIN_RESERVED) return CborTypeInvalid;
    return (CborType)(initial >> 5);
}

/* Shared by the two container readers -- identical but for the major type. */
static bool cbor_r_container(CborReader* r, uint8_t want, size_t* count) {
    uint8_t major;
    uint64_t arg;
    if(!cbor_r_head(r, &major, &arg)) return false;
    if(major != want) {
        r->error = true;
        return false;
    }
    /* A count larger than the bytes remaining cannot possibly be satisfied,
     * and rejecting it here stops a hostile header from making the caller loop
     * millions of times before failing. */
    if(arg > r->len - r->pos) {
        r->error = true;
        return false;
    }
    *count = (size_t)arg;
    return true;
}

bool cbor_r_map(CborReader* r, size_t* count) {
    return cbor_r_container(r, CborTypeMap, count);
}

bool cbor_r_array(CborReader* r, size_t* count) {
    return cbor_r_container(r, CborTypeArray, count);
}

bool cbor_r_uint(CborReader* r, uint64_t* out) {
    uint8_t major;
    uint64_t arg;
    if(!cbor_r_head(r, &major, &arg)) return false;
    if(major != CborTypeUint) {
        r->error = true;
        return false;
    }
    *out = arg;
    return true;
}

bool cbor_r_int(CborReader* r, int64_t* out) {
    uint8_t major;
    uint64_t arg;
    if(!cbor_r_head(r, &major, &arg)) return false;

    if(major == CborTypeUint) {
        if(arg > (uint64_t)INT64_MAX) {
            r->error = true;
            return false;
        }
        *out = (int64_t)arg;
        return true;
    }
    if(major == CborTypeNegint) {
        if(arg > (uint64_t)INT64_MAX) {
            r->error = true;
            return false;
        }
        *out = -1 - (int64_t)arg;
        return true;
    }

    r->error = true;
    return false;
}

bool cbor_r_bool(CborReader* r, bool* out) {
    uint8_t major;
    uint64_t arg;
    if(!cbor_r_head(r, &major, &arg)) return false;
    if(major != CborTypeSimple || (arg != CBOR_SIMPLE_FALSE && arg != CBOR_SIMPLE_TRUE)) {
        r->error = true;
        return false;
    }
    *out = (arg == CBOR_SIMPLE_TRUE);
    return true;
}

/* Shared by bstr and tstr -- the only difference is the major type. */
static bool cbor_r_string(CborReader* r, uint8_t want, const uint8_t** data, size_t* len) {
    uint8_t major;
    uint64_t arg;
    if(!cbor_r_head(r, &major, &arg)) return false;
    if(major != want || arg > r->len - r->pos) {
        r->error = true;
        return false;
    }
    *data = r->buf + r->pos;
    *len = (size_t)arg;
    r->pos += (size_t)arg;
    return true;
}

bool cbor_r_bstr(CborReader* r, const uint8_t** data, size_t* len) {
    return cbor_r_string(r, CborTypeBstr, data, len);
}

bool cbor_r_tstr(CborReader* r, const char** data, size_t* len) {
    const uint8_t* raw = NULL;
    if(!cbor_r_string(r, CborTypeTstr, &raw, len)) return false;
    *data = (const char*)raw;
    return true;
}

bool cbor_r_skip(CborReader* r) {
    /* Iterative rather than recursive: the input is attacker-controlled and a
     * deeply nested message must not be able to run the stack out. `pending`
     * counts items still owed, containers pushing their children onto it. */
    uint64_t pending = 1;
    unsigned iterations = 0;

    while(pending > 0) {
        if(++iterations > CBOR_SKIP_MAX_ITEMS) {
            r->error = true;
            return false;
        }

        uint8_t major;
        uint64_t arg;
        if(!cbor_r_head(r, &major, &arg)) return false;
        pending--;

        switch(major) {
        case CborTypeUint:
        case CborTypeNegint:
        case CborTypeSimple:
            /* Fully consumed by the head: for major 7 the additional info is
             * the simple value itself, or the width of a float we do not
             * interpret but have already stepped over. */
            break;

        case CborTypeBstr:
        case CborTypeTstr:
            if(arg > r->len - r->pos) {
                r->error = true;
                return false;
            }
            r->pos += (size_t)arg;
            break;

        case CborTypeArray:
        case CborTypeMap:
            /* A map owes two items per entry. Bounding the entry count by the
             * bytes left first keeps the doubling from overflowing. */
            if(arg > r->len - r->pos) {
                r->error = true;
                return false;
            }
            pending += (major == CborTypeMap) ? arg * 2 : arg;
            break;

        case CborTypeTag:
            pending += 1; /* the tagged item still follows */
            break;

        default:
            r->error = true;
            return false;
        }
    }

    return true;
}

bool cbor_r_done(const CborReader* r) {
    return !r->error && r->pos == r->len;
}

bool cbor_r_tstr_equals(const char* data, size_t len, const char* literal) {
    size_t literal_len = strlen(literal);
    return len == literal_len && memcmp(data, literal, len) == 0;
}
