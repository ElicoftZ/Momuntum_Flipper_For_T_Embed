/* Host tests for the CTAP2 CBOR codec.
 *
 * CBOR is pure logic with no hardware in the way, so unlike the USB layer it
 * really can be pinned down before a flash cycle is spent on it. The encoder
 * vectors below are taken from RFC 8949 Appendix A, so a passing run means the
 * output is byte-exact against the standard rather than merely self-consistent.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fido_cbor.h"

#define EXPECT(w, ...)                                              \
    do {                                                            \
        static const uint8_t expected[] = {__VA_ARGS__};            \
        size_t produced = cbor_w_finish(&(w));                      \
        assert(produced == sizeof(expected));                       \
        assert(memcmp((w).buf, expected, sizeof(expected)) == 0);   \
    } while(0)

/* ------------------------------------------------------- writer: integers */

/* RFC 8949 Appendix A. These pin the shortest-form rule, which canonical CTAP2
 * CBOR requires: an encoder that emitted 0x1817 for 23 would still round-trip
 * through our own reader but be rejected by a real client. */
static void test_write_uint_vectors(void) {
    uint8_t buf[16];
    CborWriter w;

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_uint(&w, 0);
    EXPECT(w, 0x00);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_uint(&w, 23);
    EXPECT(w, 0x17);

    /* 24 is the first value needing a trailing byte. */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_uint(&w, 24);
    EXPECT(w, 0x18, 0x18);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_uint(&w, 100);
    EXPECT(w, 0x18, 0x64);

    /* 255 -> 256 crosses into the two-byte form. */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_uint(&w, 255);
    EXPECT(w, 0x18, 0xFF);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_uint(&w, 256);
    EXPECT(w, 0x19, 0x01, 0x00);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_uint(&w, 1000);
    EXPECT(w, 0x19, 0x03, 0xE8);

    /* 65535 -> 65536 crosses into the four-byte form. */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_uint(&w, 65535);
    EXPECT(w, 0x19, 0xFF, 0xFF);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_uint(&w, 65536);
    EXPECT(w, 0x1A, 0x00, 0x01, 0x00, 0x00);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_uint(&w, 1000000);
    EXPECT(w, 0x1A, 0x00, 0x0F, 0x42, 0x40);

    /* And 2^32-1 -> 2^32 into the eight-byte form. */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_uint(&w, 0xFFFFFFFFu);
    EXPECT(w, 0x1A, 0xFF, 0xFF, 0xFF, 0xFF);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_uint(&w, 1000000000000ULL);
    EXPECT(w, 0x1B, 0x00, 0x00, 0x00, 0xE8, 0xD4, 0xA5, 0x10, 0x00);
}

static void test_write_negint_vectors(void) {
    uint8_t buf[16];
    CborWriter w;

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_int(&w, -1);
    EXPECT(w, 0x20);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_int(&w, -10);
    EXPECT(w, 0x29);

    /* -7 is ES256, the only algorithm this authenticator offers. */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_int(&w, -7);
    EXPECT(w, 0x26);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_int(&w, -100);
    EXPECT(w, 0x38, 0x63);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_int(&w, -1000);
    EXPECT(w, 0x39, 0x03, 0xE7);

    /* INT64_MIN is the case where forming the argument as -value would
     * overflow; -(value + 1) is why it does not. */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_int(&w, INT64_MIN);
    EXPECT(w, 0x3B, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);

    /* Non-negative values routed through cbor_w_int must still land in major 0. */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_int(&w, 0);
    EXPECT(w, 0x00);
}

/* ------------------------------------------- writer: strings, containers */

static void test_write_strings_and_containers(void) {
    uint8_t buf[64];
    CborWriter w;

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_tstr(&w, "");
    EXPECT(w, 0x60);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_tstr(&w, "a");
    EXPECT(w, 0x61, 0x61);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_tstr(&w, "IETF");
    EXPECT(w, 0x64, 0x49, 0x45, 0x54, 0x46);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_bstr(&w, NULL, 0);
    EXPECT(w, 0x40);

    {
        static const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
        cbor_w_init(&w, buf, sizeof(buf));
        cbor_w_bstr(&w, payload, sizeof(payload));
        EXPECT(w, 0x44, 0x01, 0x02, 0x03, 0x04);
    }

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_array(&w, 0);
    EXPECT(w, 0x80);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_array(&w, 3);
    cbor_w_uint(&w, 1);
    cbor_w_uint(&w, 2);
    cbor_w_uint(&w, 3);
    EXPECT(w, 0x83, 0x01, 0x02, 0x03);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 0);
    EXPECT(w, 0xA0);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 2);
    cbor_w_uint(&w, 1);
    cbor_w_uint(&w, 2);
    cbor_w_uint(&w, 3);
    cbor_w_uint(&w, 4);
    EXPECT(w, 0xA2, 0x01, 0x02, 0x03, 0x04);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_bool(&w, false);
    EXPECT(w, 0xF4);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_bool(&w, true);
    EXPECT(w, 0xF5);

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_null(&w);
    EXPECT(w, 0xF6);
}

/* A truncated CBOR message is worse than none: a client would parse the prefix
 * and act on a half-built response. cbor_w_finish must report 0, and the
 * poisoning must be sticky so a later small write cannot un-fail it. */
static void test_writer_overflow_is_sticky(void) {
    uint8_t buf[4];
    CborWriter w;

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_bstr(&w, "abcdefgh", 8);
    assert(w.error);
    assert(cbor_w_finish(&w) == 0);

    /* A subsequent write that would have fitted must not clear the failure. */
    cbor_w_uint(&w, 1);
    assert(cbor_w_finish(&w) == 0);

    /* Exactly filling the buffer is not an overflow. */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_bstr(&w, "abc", 3);
    assert(!w.error);
    assert(cbor_w_finish(&w) == 4);

    /* One more byte than the buffer holds is. */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_bstr(&w, "abcd", 4);
    assert(cbor_w_finish(&w) == 0);

    /* A reservation that does not fit hands back NULL rather than a pointer
     * past the end of the buffer. */
    cbor_w_init(&w, buf, sizeof(buf));
    assert(cbor_w_bstr_reserve(&w, 64) == NULL);
    assert(cbor_w_finish(&w) == 0);
}

/* authData is built in place rather than in a scratch buffer and copied. */
static void test_bstr_reserve_writes_in_place(void) {
    uint8_t buf[16];
    CborWriter w;

    cbor_w_init(&w, buf, sizeof(buf));
    uint8_t* slot = cbor_w_bstr_reserve(&w, 3);
    assert(slot != NULL);
    slot[0] = 0xAA;
    slot[1] = 0xBB;
    slot[2] = 0xCC;
    EXPECT(w, 0x43, 0xAA, 0xBB, 0xCC);
}

/* ---------------------------------------------------------------- reader */

static void test_read_scalars(void) {
    static const uint8_t encoded[] = {0x1B, 0x00, 0x00, 0x00, 0xE8, 0xD4, 0xA5, 0x10, 0x00};
    CborReader r;
    uint64_t u = 0;

    cbor_r_init(&r, encoded, sizeof(encoded));
    assert(cbor_r_peek(&r) == CborTypeUint);
    assert(cbor_r_uint(&r, &u));
    assert(u == 1000000000000ULL);
    assert(cbor_r_done(&r));

    /* cbor_r_int spans both integer majors, which is what CTAP2 map keys and
     * COSE labels need. */
    {
        static const uint8_t negative[] = {0x38, 0x63}; /* -100 */
        int64_t i = 0;
        cbor_r_init(&r, negative, sizeof(negative));
        assert(cbor_r_peek(&r) == CborTypeNegint);
        assert(cbor_r_int(&r, &i));
        assert(i == -100);
        assert(cbor_r_done(&r));
    }
    {
        static const uint8_t positive[] = {0x17}; /* 23 */
        int64_t i = 0;
        cbor_r_init(&r, positive, sizeof(positive));
        assert(cbor_r_int(&r, &i));
        assert(i == 23);
    }
    {
        static const uint8_t es256[] = {0x26}; /* -7 */
        int64_t i = 0;
        cbor_r_init(&r, es256, sizeof(es256));
        assert(cbor_r_int(&r, &i));
        assert(i == -7);
    }

    /* Reading the wrong type must fail rather than silently coerce. */
    {
        static const uint8_t text[] = {0x61, 0x61}; /* "a" */
        cbor_r_init(&r, text, sizeof(text));
        assert(!cbor_r_uint(&r, &u));
        assert(r.error);
    }

    {
        static const uint8_t booleans[] = {0xF5, 0xF4};
        bool b = false;
        cbor_r_init(&r, booleans, sizeof(booleans));
        assert(cbor_r_bool(&r, &b) && b);
        assert(cbor_r_bool(&r, &b) && !b);
        assert(cbor_r_done(&r));

        /* null is a simple value but not a boolean. */
        static const uint8_t nul[] = {0xF6};
        cbor_r_init(&r, nul, sizeof(nul));
        assert(!cbor_r_bool(&r, &b));
    }
}

static void test_read_strings_are_zero_copy(void) {
    static const uint8_t encoded[] = {0x64, 0x49, 0x45, 0x54, 0x46}; /* "IETF" */
    CborReader r;
    const char* text = NULL;
    size_t len = 0;

    cbor_r_init(&r, encoded, sizeof(encoded));
    assert(cbor_r_tstr(&r, &text, &len));
    assert(len == 4);
    /* The pointer must aim into the caller's buffer, not a copy. */
    assert(text == (const char*)encoded + 1);
    assert(cbor_r_tstr_equals(text, len, "IETF"));
    assert(!cbor_r_tstr_equals(text, len, "IET"));
    assert(!cbor_r_tstr_equals(text, len, "IETFF"));
    assert(cbor_r_done(&r));
}

/* Every one of these is a message a hostile or buggy client could send, and
 * each must fail cleanly rather than read past the buffer. */
static void test_reader_rejects_malformed(void) {
    CborReader r;
    uint64_t u = 0;
    size_t count = 0;

    /* Indefinite-length bstr / array / map, all banned by canonical CBOR. */
    {
        static const uint8_t indefinite_bstr[] = {0x5F, 0x41, 0x01, 0xFF};
        const uint8_t* data = NULL;
        size_t len = 0;
        cbor_r_init(&r, indefinite_bstr, sizeof(indefinite_bstr));
        assert(cbor_r_peek(&r) == CborTypeInvalid);
        assert(!cbor_r_bstr(&r, &data, &len));
    }
    {
        static const uint8_t indefinite_array[] = {0x9F, 0x01, 0xFF};
        cbor_r_init(&r, indefinite_array, sizeof(indefinite_array));
        assert(!cbor_r_array(&r, &count));
    }
    {
        static const uint8_t indefinite_map[] = {0xBF, 0x01, 0x02, 0xFF};
        cbor_r_init(&r, indefinite_map, sizeof(indefinite_map));
        assert(!cbor_r_map(&r, &count));
    }

    /* Reserved additional-info values 28..30. */
    {
        static const uint8_t reserved[] = {0x1C};
        cbor_r_init(&r, reserved, sizeof(reserved));
        assert(cbor_r_peek(&r) == CborTypeInvalid);
        assert(!cbor_r_uint(&r, &u));
    }

    /* A head promising trailing bytes that are not there. */
    {
        static const uint8_t truncated[] = {0x19, 0x01};
        cbor_r_init(&r, truncated, sizeof(truncated));
        assert(!cbor_r_uint(&r, &u));
    }

    /* A byte string longer than the bytes that remain. */
    {
        static const uint8_t overlong[] = {0x58, 0x40, 0x01, 0x02};
        const uint8_t* data = NULL;
        size_t len = 0;
        cbor_r_init(&r, overlong, sizeof(overlong));
        assert(!cbor_r_bstr(&r, &data, &len));
    }

    /* A map header claiming more entries than could possibly fit. This is the
     * cheap denial-of-service shape: two bytes asking the caller to loop
     * 65535 times. It must be refused at the header. */
    {
        static const uint8_t huge_map[] = {0xB9, 0xFF, 0xFF};
        cbor_r_init(&r, huge_map, sizeof(huge_map));
        assert(!cbor_r_map(&r, &count));
    }

    /* An empty buffer. */
    cbor_r_init(&r, NULL, 0);
    assert(cbor_r_peek(&r) == CborTypeInvalid);
    assert(!cbor_r_uint(&r, &u));
}

/* CTAP2 clients send map entries and extensions this firmware does not
 * implement, so stepping over an arbitrary value is a hot path, not an edge. */
static void test_skip(void) {
    CborReader r;
    uint64_t u = 0;

    /* Skip each scalar shape, then read the sentinel that follows it. */
    {
        static const uint8_t encoded[] = {
            0x1A, 0x00, 0x0F, 0x42, 0x40, /* 1000000  */
            0x38, 0x63, /* -100     */
            0x64, 0x49, 0x45, 0x54, 0x46, /* "IETF"   */
            0x44, 0x01, 0x02, 0x03, 0x04, /* h'01020304' */
            0xF5, /* true     */
            0xF6, /* null     */
            0x07, /* sentinel */
        };
        cbor_r_init(&r, encoded, sizeof(encoded));
        for(int i = 0; i < 6; i++) assert(cbor_r_skip(&r));
        assert(cbor_r_uint(&r, &u) && u == 7);
        assert(cbor_r_done(&r));
    }

    /* Skip a nested container: {1: [1, 2, {3: "x"}], 2: h'AA'} then a sentinel. */
    {
        static const uint8_t encoded[] = {
            0xA2, /* map(2)          */
            0x01, /*   key 1         */
            0x83, /*   array(3)      */
            0x01, 0x02, /*     1, 2        */
            0xA1, 0x03, 0x61, 0x78, /*     {3: "x"}    */
            0x02, /*   key 2         */
            0x41, 0xAA, /*   h'AA'         */
            0x07, /* sentinel        */
        };
        cbor_r_init(&r, encoded, sizeof(encoded));
        assert(cbor_r_skip(&r));
        assert(cbor_r_uint(&r, &u) && u == 7);
        assert(cbor_r_done(&r));
    }

    /* A tag must carry its tagged item along with it. */
    {
        static const uint8_t encoded[] = {0xC1, 0x1A, 0x51, 0x4B, 0x67, 0xB0, 0x07};
        cbor_r_init(&r, encoded, sizeof(encoded));
        assert(cbor_r_skip(&r));
        assert(cbor_r_uint(&r, &u) && u == 7);
        assert(cbor_r_done(&r));
    }

    /* Skipping a container whose declared count cannot be satisfied must fail
     * rather than run off the end. */
    {
        static const uint8_t encoded[] = {0x84, 0x01, 0x02};
        cbor_r_init(&r, encoded, sizeof(encoded));
        assert(!cbor_r_skip(&r));
    }

    /* Skipping past the end of the buffer. */
    {
        static const uint8_t encoded[] = {0x01};
        cbor_r_init(&r, encoded, sizeof(encoded));
        assert(cbor_r_skip(&r));
        assert(!cbor_r_skip(&r));
    }
}

/* The shape the CTAP2 parser actually meets: an integer-keyed map holding
 * mixed values, some of which it does not care about and must step over. */
static void test_ctap2_shaped_round_trip(void) {
    static const uint8_t client_data_hash[32] = {
        0x68, 0x71, 0x94, 0xA6, 0x1D, 0x5C, 0x27, 0x3B, 0xB8, 0x0F, 0x9E,
        0x33, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x02, 0x03, 0x04,
    };
    uint8_t buf[256];
    CborWriter w;

    /* Roughly an authenticatorMakeCredential request:
     *   1: clientDataHash, 2: {id, name}, 4: [{alg, type}], 7: {rk: true} */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 4);

    cbor_w_uint(&w, 1);
    cbor_w_bstr(&w, client_data_hash, sizeof(client_data_hash));

    cbor_w_uint(&w, 2);
    cbor_w_map(&w, 2);
    cbor_w_tstr(&w, "id");
    cbor_w_tstr(&w, "webauthn.io");
    cbor_w_tstr(&w, "name");
    cbor_w_tstr(&w, "webauthn.io");

    cbor_w_uint(&w, 4);
    cbor_w_array(&w, 1);
    cbor_w_map(&w, 2);
    cbor_w_tstr(&w, "alg");
    cbor_w_int(&w, -7);
    cbor_w_tstr(&w, "type");
    cbor_w_tstr(&w, "public-key");

    cbor_w_uint(&w, 7);
    cbor_w_map(&w, 1);
    cbor_w_tstr(&w, "rk");
    cbor_w_bool(&w, true);

    size_t encoded_len = cbor_w_finish(&w);
    assert(encoded_len > 0);

    /* Parse it back the way ctap2.c will: walk the outer map, handle the keys
     * that matter, skip the rest. */
    CborReader r;
    cbor_r_init(&r, buf, encoded_len);

    size_t entries = 0;
    assert(cbor_r_map(&r, &entries));
    assert(entries == 4);

    bool saw_hash = false;
    bool saw_rp = false;
    bool saw_alg = false;
    bool saw_rk = false;

    for(size_t i = 0; i < entries; i++) {
        uint64_t key = 0;
        assert(cbor_r_uint(&r, &key));

        if(key == 1) {
            const uint8_t* hash = NULL;
            size_t hash_len = 0;
            assert(cbor_r_bstr(&r, &hash, &hash_len));
            assert(hash_len == sizeof(client_data_hash));
            assert(memcmp(hash, client_data_hash, hash_len) == 0);
            saw_hash = true;

        } else if(key == 2) {
            size_t fields = 0;
            assert(cbor_r_map(&r, &fields));
            for(size_t f = 0; f < fields; f++) {
                const char* name = NULL;
                size_t name_len = 0;
                assert(cbor_r_tstr(&r, &name, &name_len));
                if(cbor_r_tstr_equals(name, name_len, "id")) {
                    const char* value = NULL;
                    size_t value_len = 0;
                    assert(cbor_r_tstr(&r, &value, &value_len));
                    assert(cbor_r_tstr_equals(value, value_len, "webauthn.io"));
                    saw_rp = true;
                } else {
                    /* "name" is not needed -- step over it. */
                    assert(cbor_r_skip(&r));
                }
            }

        } else if(key == 4) {
            size_t params = 0;
            assert(cbor_r_array(&r, &params));
            assert(params == 1);
            size_t fields = 0;
            assert(cbor_r_map(&r, &fields));
            for(size_t f = 0; f < fields; f++) {
                const char* name = NULL;
                size_t name_len = 0;
                assert(cbor_r_tstr(&r, &name, &name_len));
                if(cbor_r_tstr_equals(name, name_len, "alg")) {
                    int64_t alg = 0;
                    assert(cbor_r_int(&r, &alg));
                    assert(alg == -7);
                    saw_alg = true;
                } else {
                    assert(cbor_r_skip(&r));
                }
            }

        } else if(key == 7) {
            size_t options = 0;
            assert(cbor_r_map(&r, &options));
            for(size_t o = 0; o < options; o++) {
                const char* name = NULL;
                size_t name_len = 0;
                assert(cbor_r_tstr(&r, &name, &name_len));
                if(cbor_r_tstr_equals(name, name_len, "rk")) {
                    bool rk = false;
                    assert(cbor_r_bool(&r, &rk));
                    assert(rk);
                    saw_rk = true;
                } else {
                    assert(cbor_r_skip(&r));
                }
            }

        } else {
            assert(cbor_r_skip(&r));
        }
    }

    assert(saw_hash && saw_rp && saw_alg && saw_rk);
    assert(cbor_r_done(&r));
}

int main(void) {
    test_write_uint_vectors();
    test_write_negint_vectors();
    test_write_strings_and_containers();
    test_writer_overflow_is_sticky();
    test_bstr_reserve_writes_in_place();
    test_read_scalars();
    test_read_strings_are_zero_copy();
    test_reader_rejects_malformed();
    test_skip();
    test_ctap2_shaped_round_trip();
    puts("FIDO CBOR host tests passed.");
    return 0;
}
