/* Host-side generator input for mquickjs's build_atoms() tool.
 *
 * This is Phase 1: no custom native bindings yet, just an empty stdlib table
 * so we can prove the engine itself (parse+exec+arena) compiles and runs on
 * this hardware before any namespace binding work starts. Later phases add
 * entries to global_object[]/c_function_decl[] the same way Bruce's own
 * mqjs_stdlib.c does (see Bruce-Firmware/src/modules/bjs_interpreter/mqjs_stdlib.c
 * for the reference pattern once real bindings are added).
 *
 * This file is compiled and run on the HOST (not the ESP32 target) as part of
 * the build; its stdout is captured into a generated header consumed by the
 * ESP-IDF build. See components/mquickjs/CMakeLists.txt.
 */
#include <math.h>

#include "mquickjs_build.h"

/* ---- Core JS language objects -----------------------------------------
 *
 * Found while adding wifi.httpFetch(): ANY property access on a string/
 * number/boolean PRIMITIVE that mquickjs can't answer from its own fast path
 * (i.e. anything beyond a string's .length) segfaults the whole firmware --
 * "hi".charAt(0), (5).toFixed(2), true.foo, even "hi".nonexistentMethod --
 * because JS_GetPropertyInternal()'s primitive-boxing fallback dereferences
 * ctx->class_proto[JS_CLASS_STRING/NUMBER/BOOLEAN], which was NULL: this
 * port's stdlib table never registered Object/Function/Number/Boolean/
 * String/Array/Error at all, only the Bruce-specific namespaces below. Every
 * one of these is a real, already-compiled mquickjs.c internal (same
 * "compiled in but never exposed" situation Phase 3.5 already found for
 * Math/JSON/Uint8Array) -- this is Bruce's own REAL reference registration
 * table, copied verbatim from the vendored lib/mquickjs/mqjs_stdlib.c (the
 * engine author's own REPL/reference stdlib, not Bruce-firmware-specific --
 * these are just core JavaScript, needed regardless of which product embeds
 * this engine). Registering these doesn't just add compatibility, it fixes a
 * standing crash: almost any non-trivial real script eventually calls a
 * string method or touches a boxed number/boolean.
 *
 * Deliberately NOT ported from mqjs_stdlib.c: Date/console/performance/print
 * (their native bodies -- js_date_now, js_print, js_performance_now -- live
 * in mqjs.c/example.c, the standalone REPL binary, not in the portable
 * mquickjs.c/cutils.c/dtoa.c/libm.c core this component actually compiles;
 * registering them here would be a link error, not just a gap) and
 * eval/load/gc/setTimeout/clearTimeout (REPL/host conveniences with no safe
 * embedded equivalent here -- this port's own now()/delay()/random() below
 * already cover a real Bruce script's actual timing needs). Math is
 * deliberately NOT replaced with mqjs_stdlib.c's fuller table either: that
 * one uses the "f_f" special-cproto convention (raw libm function pointers,
 * a different calling convention than the plain generic one every other
 * binding here uses) which hasn't been verified to work with this
 * component's build_atoms() -- the narrower native_math_* table further
 * below is hand-written but proven working on real hardware; revisit if a
 * script ever needs a Math function it doesn't cover. */
static const JSPropDef js_object_proto[] = {
    JS_CFUNC_DEF("hasOwnProperty", 1, js_object_hasOwnProperty),
    JS_CFUNC_DEF("toString", 0, js_object_toString),
    JS_PROP_END,
};
static const JSPropDef js_object[] = {
    JS_CFUNC_DEF("defineProperty", 3, js_object_defineProperty),
    JS_CFUNC_DEF("getPrototypeOf", 1, js_object_getPrototypeOf),
    JS_CFUNC_DEF("setPrototypeOf", 2, js_object_setPrototypeOf),
    JS_CFUNC_DEF("create", 2, js_object_create),
    JS_CFUNC_DEF("keys", 1, js_object_keys),
    JS_PROP_END,
};
static const JSClassDef js_object_class = JS_CLASS_DEF(
    "Object", 1, js_object_constructor, JS_CLASS_OBJECT, js_object, js_object_proto, NULL, NULL);

static const JSPropDef js_function_proto[] = {
    JS_CGETSET_DEF("prototype", js_function_get_prototype, js_function_set_prototype),
    JS_CFUNC_DEF("call", 1, js_function_call),
    JS_CFUNC_DEF("apply", 2, js_function_apply),
    JS_CFUNC_DEF("bind", 1, js_function_bind),
    JS_CFUNC_DEF("toString", 0, js_function_toString),
    JS_CGETSET_MAGIC_DEF("length", js_function_get_length_name, NULL, 0),
    JS_CGETSET_MAGIC_DEF("name", js_function_get_length_name, NULL, 1),
    JS_PROP_END,
};
static const JSClassDef js_function_class = JS_CLASS_DEF(
    "Function", 1, js_function_constructor, JS_CLASS_CLOSURE, NULL, js_function_proto, NULL, NULL);

static const JSPropDef js_number_proto[] = {
    JS_CFUNC_DEF("toExponential", 1, js_number_toExponential),
    JS_CFUNC_DEF("toFixed", 1, js_number_toFixed),
    JS_CFUNC_DEF("toPrecision", 1, js_number_toPrecision),
    JS_CFUNC_DEF("toString", 1, js_number_toString),
    JS_PROP_END,
};
static const JSPropDef js_number[] = {
    JS_CFUNC_DEF("parseInt", 2, js_number_parseInt),
    JS_CFUNC_DEF("parseFloat", 1, js_number_parseFloat),
    JS_PROP_DOUBLE_DEF("MAX_VALUE", 1.7976931348623157e+308, 0),
    JS_PROP_DOUBLE_DEF("MIN_VALUE", 5e-324, 0),
    JS_PROP_DOUBLE_DEF("NaN", NAN, 0),
    JS_PROP_DOUBLE_DEF("NEGATIVE_INFINITY", -INFINITY, 0),
    JS_PROP_DOUBLE_DEF("POSITIVE_INFINITY", INFINITY, 0),
    JS_PROP_DOUBLE_DEF("EPSILON", 2.220446049250313e-16, 0),
    JS_PROP_DOUBLE_DEF("MAX_SAFE_INTEGER", 9007199254740991.0, 0),
    JS_PROP_DOUBLE_DEF("MIN_SAFE_INTEGER", -9007199254740991.0, 0),
    JS_PROP_END,
};
static const JSClassDef js_number_class = JS_CLASS_DEF(
    "Number", 1, js_number_constructor, JS_CLASS_NUMBER, js_number, js_number_proto, NULL, NULL);

static const JSClassDef js_boolean_class = JS_CLASS_DEF(
    "Boolean", 1, js_boolean_constructor, JS_CLASS_BOOLEAN, NULL, NULL, NULL, NULL);

static const JSPropDef js_string_proto[] = {
    JS_CGETSET_DEF("length", js_string_get_length, js_string_set_length),
    JS_CFUNC_MAGIC_DEF("charAt", 1, js_string_charAt, magic_charAt),
    JS_CFUNC_MAGIC_DEF("charCodeAt", 1, js_string_charAt, magic_charCodeAt),
    JS_CFUNC_MAGIC_DEF("codePointAt", 1, js_string_charAt, magic_codePointAt),
    JS_CFUNC_DEF("slice", 2, js_string_slice),
    JS_CFUNC_DEF("substring", 2, js_string_substring),
    JS_CFUNC_DEF("concat", 1, js_string_concat),
    JS_CFUNC_MAGIC_DEF("indexOf", 1, js_string_indexOf, 0),
    JS_CFUNC_MAGIC_DEF("lastIndexOf", 1, js_string_indexOf, 1),
    JS_CFUNC_DEF("match", 1, js_string_match),
    JS_CFUNC_MAGIC_DEF("replace", 2, js_string_replace, 0),
    JS_CFUNC_MAGIC_DEF("replaceAll", 2, js_string_replace, 1),
    JS_CFUNC_DEF("search", 1, js_string_search),
    JS_CFUNC_DEF("split", 2, js_string_split),
    JS_CFUNC_MAGIC_DEF("toLowerCase", 0, js_string_toLowerCase, 1),
    JS_CFUNC_MAGIC_DEF("toUpperCase", 0, js_string_toLowerCase, 0),
    JS_CFUNC_MAGIC_DEF("trim", 0, js_string_trim, 3),
    JS_CFUNC_MAGIC_DEF("trimEnd", 0, js_string_trim, 2),
    JS_CFUNC_MAGIC_DEF("trimStart", 0, js_string_trim, 1),
    JS_CFUNC_DEF("toString", 0, js_string_toString),
    JS_CFUNC_DEF("repeat", 1, js_string_repeat),
    JS_PROP_END,
};
static const JSPropDef js_string[] = {
    JS_CFUNC_MAGIC_DEF("fromCharCode", 1, js_string_fromCharCode, 0),
    JS_CFUNC_MAGIC_DEF("fromCodePoint", 1, js_string_fromCharCode, 1),
    JS_PROP_END,
};
static const JSClassDef js_string_class = JS_CLASS_DEF(
    "String", 1, js_string_constructor, JS_CLASS_STRING, js_string, js_string_proto, NULL, NULL);

static const JSPropDef js_array_proto[] = {
    JS_CFUNC_DEF("concat", 1, js_array_concat),
    JS_CGETSET_DEF("length", js_array_get_length, js_array_set_length),
    JS_CFUNC_MAGIC_DEF("push", 1, js_array_push, 0),
    JS_CFUNC_DEF("pop", 0, js_array_pop),
    JS_CFUNC_DEF("join", 1, js_array_join),
    JS_CFUNC_DEF("toString", 0, js_array_toString),
    JS_CFUNC_DEF("reverse", 0, js_array_reverse),
    JS_CFUNC_DEF("shift", 0, js_array_shift),
    JS_CFUNC_DEF("slice", 2, js_array_slice),
    JS_CFUNC_DEF("splice", 2, js_array_splice),
    JS_CFUNC_MAGIC_DEF("unshift", 1, js_array_push, 1),
    JS_CFUNC_MAGIC_DEF("indexOf", 1, js_array_indexOf, 0),
    JS_CFUNC_MAGIC_DEF("lastIndexOf", 1, js_array_indexOf, 1),
    JS_CFUNC_MAGIC_DEF("every", 1, js_array_every, js_special_every),
    JS_CFUNC_MAGIC_DEF("some", 1, js_array_every, js_special_some),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_array_every, js_special_forEach),
    JS_CFUNC_MAGIC_DEF("map", 1, js_array_every, js_special_map),
    JS_CFUNC_MAGIC_DEF("filter", 1, js_array_every, js_special_filter),
    JS_CFUNC_MAGIC_DEF("reduce", 1, js_array_reduce, js_special_reduce),
    JS_CFUNC_MAGIC_DEF("reduceRight", 1, js_array_reduce, js_special_reduceRight),
    JS_CFUNC_DEF("sort", 1, js_array_sort),
    JS_PROP_END,
};
static const JSPropDef js_array[] = {
    JS_CFUNC_DEF("isArray", 1, js_array_isArray),
    JS_PROP_END,
};
static const JSClassDef js_array_class = JS_CLASS_DEF(
    "Array", 1, js_array_constructor, JS_CLASS_ARRAY, js_array, js_array_proto, NULL, NULL);

static const JSPropDef js_error_proto[] = {
    JS_CFUNC_DEF("toString", 0, js_error_toString),
    JS_PROP_STRING_DEF("name", "Error", 0),
    JS_CGETSET_MAGIC_DEF("message", js_error_get_message, NULL, 0),
    JS_CGETSET_MAGIC_DEF("stack", js_error_get_message, NULL, 1),
    JS_PROP_END,
};
static const JSClassDef js_error_class = JS_CLASS_MAGIC_DEF(
    "Error", 1, js_error_constructor, JS_CLASS_ERROR, NULL, js_error_proto, NULL, NULL);

#define ERROR_DEF(cname, name, class_id)                                                       \
    static const JSPropDef js_##cname##_proto[] = {                                            \
        JS_PROP_STRING_DEF("name", name, 0),                                                   \
        JS_PROP_END,                                                                           \
    };                                                                                          \
    static const JSClassDef js_##cname##_class = JS_CLASS_MAGIC_DEF(                           \
        name, 1, js_error_constructor, class_id, NULL, js_##cname##_proto, &js_error_class, NULL);

ERROR_DEF(eval_error, "EvalError", JS_CLASS_EVAL_ERROR)
ERROR_DEF(range_error, "RangeError", JS_CLASS_RANGE_ERROR)
ERROR_DEF(reference_error, "ReferenceError", JS_CLASS_REFERENCE_ERROR)
ERROR_DEF(syntax_error, "SyntaxError", JS_CLASS_SYNTAX_ERROR)
ERROR_DEF(type_error, "TypeError", JS_CLASS_TYPE_ERROR)
ERROR_DEF(uri_error, "URIError", JS_CLASS_URI_ERROR)
ERROR_DEF(internal_error, "InternalError", JS_CLASS_INTERNAL_ERROR)

/* ArrayBuffer + all 9 real typed array views (not just Uint8Array -- Bruce
 * scripts embedding sprite/audio/binary data can use any of these). Same
 * `.fill()`-dropped TypedArray base proto as before: this engine build has
 * no `js_fill` symbol at all (confirmed absent by grep), so that one method
 * from Bruce's real table can't be copied verbatim -- everything else
 * (constructor, indexed access, subarray/set/join/toString) doesn't depend
 * on it. */
static const JSPropDef js_array_buffer_proto[] = {
    JS_CGETSET_DEF("byteLength", js_array_buffer_get_byteLength, NULL),
    JS_PROP_END,
};
static const JSClassDef js_array_buffer_class = JS_CLASS_DEF(
    "ArrayBuffer",
    1,
    js_array_buffer_constructor,
    JS_CLASS_ARRAY_BUFFER,
    NULL,
    js_array_buffer_proto,
    NULL,
    NULL);

static const JSPropDef js_typed_array_base_proto[] = {
    JS_CGETSET_MAGIC_DEF("length", js_typed_array_get_length, NULL, 0),
    JS_CGETSET_MAGIC_DEF("byteLength", js_typed_array_get_length, NULL, 1),
    JS_CGETSET_MAGIC_DEF("byteOffset", js_typed_array_get_length, NULL, 2),
    JS_CGETSET_MAGIC_DEF("buffer", js_typed_array_get_length, NULL, 3),
    JS_CFUNC_DEF("join", 1, js_array_join),
    JS_CFUNC_DEF("toString", 0, js_array_toString),
    JS_CFUNC_DEF("subarray", 2, js_typed_array_subarray),
    JS_CFUNC_DEF("set", 1, js_typed_array_set),
    JS_PROP_END,
};
static const JSClassDef js_typed_array_base_class = JS_CLASS_DEF(
    "TypedArray",
    0,
    js_typed_array_base_constructor,
    JS_CLASS_TYPED_ARRAY,
    NULL,
    js_typed_array_base_proto,
    NULL,
    NULL);

#define TA_DEF(name, class_name, bpe)                                                          \
    static const JSPropDef js_##name[] = {                                                     \
        JS_PROP_DOUBLE_DEF("BYTES_PER_ELEMENT", bpe, 0),                                       \
        JS_PROP_END,                                                                           \
    };                                                                                          \
    static const JSPropDef js_##name##_proto[] = {                                             \
        JS_PROP_DOUBLE_DEF("BYTES_PER_ELEMENT", bpe, 0),                                       \
        JS_PROP_END,                                                                           \
    };                                                                                          \
    static const JSClassDef js_##name##_class = JS_CLASS_MAGIC_DEF(                            \
        #name,                                                                                 \
        3,                                                                                      \
        js_typed_array_constructor,                                                            \
        class_name,                                                                             \
        js_##name,                                                                              \
        js_##name##_proto,                                                                      \
        &js_typed_array_base_class,                                                            \
        NULL);

TA_DEF(Uint8ClampedArray, JS_CLASS_UINT8C_ARRAY, 1)
TA_DEF(Int8Array, JS_CLASS_INT8_ARRAY, 1)
TA_DEF(Uint8Array, JS_CLASS_UINT8_ARRAY, 1)
TA_DEF(Int16Array, JS_CLASS_INT16_ARRAY, 2)
TA_DEF(Uint16Array, JS_CLASS_UINT16_ARRAY, 2)
TA_DEF(Int32Array, JS_CLASS_INT32_ARRAY, 4)
TA_DEF(Uint32Array, JS_CLASS_UINT32_ARRAY, 4)
TA_DEF(Float32Array, JS_CLASS_FLOAT32_ARRAY, 4)
TA_DEF(Float64Array, JS_CLASS_FLOAT64_ARRAY, 8)

static const JSPropDef js_regexp_proto[] = {
    JS_CGETSET_DEF("lastIndex", js_regexp_get_lastIndex, js_regexp_set_lastIndex),
    JS_CGETSET_DEF("source", js_regexp_get_source, NULL),
    JS_CGETSET_DEF("flags", js_regexp_get_flags, NULL),
    JS_CFUNC_MAGIC_DEF("exec", 1, js_regexp_exec, 0),
    JS_CFUNC_MAGIC_DEF("test", 1, js_regexp_exec, 1),
    JS_PROP_END,
};
static const JSClassDef js_regexp_class = JS_CLASS_DEF(
    "RegExp", 2, js_regexp_constructor, JS_CLASS_REGEXP, NULL, js_regexp_proto, NULL, NULL);

/* Phase 2, first real binding: subghz.isFrequencyValid(freq) -- proves the
 * full custom-binding pipeline (this table -> generated header -> native C
 * function -> JS-callable) using a safe, read-only call into the same
 * lib/subghz stack ProtoPirate/js_subghz already use. See
 * applications/system/bruce_js_app/bruce_js_app.c for the native
 * implementation this table's string names resolve to. */
static const JSPropDef js_subghz[] = {
    JS_CFUNC_DEF("isFrequencyValid", 1, native_subghz_is_frequency_valid),
    JS_PROP_END,
};
static const JSClassDef js_subghz_obj = JS_OBJECT_DEF("SubGhz", js_subghz);

/* Phase 3, first hard-namespace binding: gpio.pinMode/digitalWrite/
 * digitalRead. This port's furi_hal_gpio.c already has real init/write/read
 * (unlike i2c/spi/serial, which have no implementation at all yet -- see
 * the plan). CLI-only, never auto-run: writing to an arbitrary pin could
 * clash with LCD/SD/radio/etc. wiring, so this must only run when a user
 * explicitly names a pin they know is safe to touch. */
static const JSPropDef js_gpio[] = {
    JS_CFUNC_DEF("pinMode", 2, native_gpio_pin_mode),
    JS_CFUNC_DEF("digitalWrite", 2, native_gpio_digital_write),
    JS_CFUNC_DEF("digitalRead", 1, native_gpio_digital_read),
    JS_PROP_END,
};
static const JSClassDef js_gpio_obj = JS_OBJECT_DEF("GPIO", js_gpio);

/* device.getFreeHeap()/getUptime() -- zero hardware risk, pure info getters,
 * matching a safe subset of Bruce's own device.* namespace. getBoard/getModel
 * match Bruce-Firmware/src/modules/bjs_interpreter/mqjs_stdlib.c's own table
 * exactly (both names bound to the same function there too) -- lets real
 * Bruce scripts, or a future Bruce App Store, detect this is a T-Embed
 * CC1101 and pick the correct hardware variant/driver. */
static const JSPropDef js_device[] = {
    JS_CFUNC_DEF("getFreeHeap", 0, native_device_get_free_heap),
    JS_CFUNC_DEF("getUptime", 0, native_device_get_uptime),
    JS_CFUNC_DEF("getBoard", 0, native_device_get_board),
    JS_CFUNC_DEF("getModel", 0, native_device_get_board),
    JS_PROP_END,
};
static const JSClassDef js_device_obj = JS_OBJECT_DEF("Device", js_device);

/* storage.read/write/remove/readdir -- the 4 methods the official Bruce App
 * Store script actually calls (see bruce_js_app.c's native_storage_* for the
 * full compatibility-scoping rationale). */
static const JSPropDef js_storage[] = {
    JS_CFUNC_DEF("read", 1, native_storage_read),
    JS_CFUNC_DEF("write", 3, native_storage_write),
    JS_CFUNC_DEF("remove", 1, native_storage_remove),
    JS_CFUNC_DEF("readdir", 2, native_storage_readdir),
    JS_PROP_END,
};
static const JSClassDef js_storage_obj = JS_OBJECT_DEF("Storage", js_storage);

/* keyboard.* -- Bruce's own abstraction over its physical buttons. This
 * board only has Up/Down/OK/Back (see momentum-port-architecture-notes
 * memory), mapped Esc=Back, Next=Down, Prev=Up, Sel=OK, matching how a real
 * Bruce Cardputer/T-Deck's directional keys would map. Edge-triggered (a
 * getXPress() call returns true only once per physical press, not
 * continuously while held) via a script-lifetime input queue -- see
 * bruce_js_app.c's BruceJsScriptCtx. */
static const JSPropDef js_keyboard[] = {
    JS_CFUNC_DEF("setLongPress", 1, native_keyboard_set_long_press),
    JS_CFUNC_DEF("getPrevPress", 0, native_keyboard_get_prev_press),
    JS_CFUNC_DEF("getNextPress", 0, native_keyboard_get_next_press),
    JS_CFUNC_DEF("getSelPress", 0, native_keyboard_get_sel_press),
    JS_CFUNC_DEF("getEscPress", 0, native_keyboard_get_esc_press),
    JS_CFUNC_DEF("getAnyPress", 0, native_keyboard_get_any_press),
    JS_PROP_END,
};
static const JSClassDef js_keyboard_obj = JS_OBJECT_DEF("Keyboard", js_keyboard);

/* display.* -- draws into a script-owned RGB565 framebuffer (see
 * bruce_js_app.c's BruceJsScriptCtx), blitted to the real panel each time
 * delay() is called (the natural once-per-frame point in a real Bruce game
 * loop) rather than after every single draw call. createSprite() is a
 * deliberate simplification: real Bruce returns a distinct off-screen Sprite
 * CLASS instance (its own prototype/finalizer) for manual double-buffering;
 * this returns the SAME global `display` object instead of building a full
 * class binding, so `sprite.drawText(...)`-style calls still work (drawing
 * straight to the one shared framebuffer) even though true multi-buffer
 * scripts won't double-buffer correctly. Good enough for scripts that treat
 * the "sprite" as just another drawing surface, which covers most simple
 * scripts; a real Sprite class is future work if a script actually needs
 * independent buffers. */
static const JSPropDef js_display[] = {
    JS_CFUNC_DEF("color", 4, native_display_color),
    JS_CFUNC_DEF("width", 0, native_display_width),
    JS_CFUNC_DEF("height", 0, native_display_height),
    JS_CFUNC_DEF("setTextColor", 1, native_display_set_text_color),
    JS_CFUNC_DEF("setTextSize", 1, native_display_set_text_size),
    JS_CFUNC_DEF("setTextAlign", 2, native_display_set_text_align),
    JS_CFUNC_DEF("drawText", 3, native_display_draw_text),
    JS_CFUNC_DEF("drawRect", 5, native_display_draw_rect),
    JS_CFUNC_DEF("drawFillRect", 5, native_display_draw_fill_rect),
    JS_CFUNC_DEF("fill", 1, native_display_fill),
    JS_CFUNC_DEF("drawXBitmap", 6, native_display_draw_xbitmap),
    JS_CFUNC_DEF("pushSprite", 0, native_display_push_sprite),
    JS_CFUNC_DEF("createSprite", 2, native_display_create_sprite),
    JS_PROP_END,
};
static const JSClassDef js_display_obj = JS_OBJECT_DEF("Display", js_display);

/* audio.tone() -- this board has no dedicated piezo buzzer wired the way
 * Bruce's own hardware does; the closest real equivalent already in this
 * port is the WS2812 status LED ring (furi_hal_light), which can't make
 * sound at all. Rather than silently pretend a beep happened, this is a
 * documented, deliberate no-op (still callable so scripts that call
 * audio.tone() don't crash) until a real speaker/PWM-buzzer path exists on
 * this board. */
static const JSPropDef js_audio[] = {
    JS_CFUNC_DEF("tone", 2, native_audio_tone),
    JS_PROP_END,
};
static const JSClassDef js_audio_obj = JS_OBJECT_DEF("Audio", js_audio);

/* Math.* -- see native_math_abs()'s comment in bruce_js_app.c for why this
 * (not just require()/Uint8Array) was needed: this engine build has no
 * global `Math` object at all by default. */
static const JSPropDef js_math[] = {
    JS_PROP_DOUBLE_DEF("PI", 3.14159265358979323846, 0),
    JS_CFUNC_DEF("abs", 1, native_math_abs),
    JS_CFUNC_DEF("floor", 1, native_math_floor),
    JS_CFUNC_DEF("ceil", 1, native_math_ceil),
    JS_CFUNC_DEF("round", 1, native_math_round),
    JS_CFUNC_DEF("sqrt", 1, native_math_sqrt),
    JS_CFUNC_DEF("min", 2, native_math_min),
    JS_CFUNC_DEF("max", 2, native_math_max),
    JS_CFUNC_DEF("trunc", 1, native_math_trunc),
    JS_CFUNC_DEF("pow", 2, native_math_pow),
    JS_CFUNC_DEF("sign", 1, native_math_sign),
    JS_CFUNC_DEF("log", 1, native_math_log),
    JS_CFUNC_DEF("exp", 1, native_math_exp),
    JS_PROP_END,
};
static const JSClassDef js_math_obj = JS_OBJECT_DEF("Math", js_math);

/* wifi.connected()/httpFetch() -- see native_wifi_connected()'s own comment
 * in bruce_js_app.c for why this deliberately never calls wifi_enable()
 * itself: this port's WiFi and BLE are mutually exclusive at the OS level
 * (see the ble-wifi-mutual-exclusion memory), and only the user explicitly
 * turning WiFi on should ever trigger that radio switch, not a script. */
static const JSPropDef js_wifi[] = {
    JS_CFUNC_DEF("connected", 0, native_wifi_connected),
    JS_CFUNC_DEF("httpFetch", 2, native_wifi_http_fetch),
    JS_PROP_END,
};
static const JSClassDef js_wifi_obj = JS_OBJECT_DEF("Wifi", js_wifi);

/* JSON.parse/stringify -- real internal mquickjs engine functions (see
 * mquickjs_priv.h), the same "compiled in but never exposed as a JS global"
 * situation Phase 3.5 already found for Math and typed arrays. Needed by
 * wifi.httpFetch's own responseType:"json" handling (native_wifi_http_fetch
 * calls js_json_parse() directly) and by any real script doing its own
 * JSON.stringify(...) for a POST body or general data work. */
static const JSPropDef js_json[] = {
    JS_CFUNC_DEF("parse", 2, js_json_parse),
    JS_CFUNC_DEF("stringify", 3, js_json_stringify),
    JS_PROP_END,
};
static const JSClassDef js_json_obj = JS_OBJECT_DEF("JSON", js_json);

/* console.log -- see native_console_log()'s own comment in bruce_js_app.c
 * for why this is hand-written rather than mqjs_stdlib.c's real js_print
 * (which lives in the REPL binary this component doesn't compile). */
static const JSPropDef js_console[] = {
    JS_CFUNC_DEF("log", 1, native_console_log),
    JS_PROP_END,
};
static const JSClassDef js_console_obj = JS_OBJECT_DEF("Console", js_console);

static const JSPropDef global_object[] = {
    /* Core JS language -- see the big comment above for why these were
     * missing and what crash that caused. */
    JS_PROP_CLASS_DEF("Object", &js_object_class),
    JS_PROP_CLASS_DEF("Function", &js_function_class),
    JS_PROP_CLASS_DEF("Number", &js_number_class),
    JS_PROP_CLASS_DEF("Boolean", &js_boolean_class),
    JS_PROP_CLASS_DEF("String", &js_string_class),
    JS_PROP_CLASS_DEF("Array", &js_array_class),
    JS_PROP_CLASS_DEF("RegExp", &js_regexp_class),
    JS_PROP_CLASS_DEF("Error", &js_error_class),
    JS_PROP_CLASS_DEF("EvalError", &js_eval_error_class),
    JS_PROP_CLASS_DEF("RangeError", &js_range_error_class),
    JS_PROP_CLASS_DEF("ReferenceError", &js_reference_error_class),
    JS_PROP_CLASS_DEF("SyntaxError", &js_syntax_error_class),
    JS_PROP_CLASS_DEF("TypeError", &js_type_error_class),
    JS_PROP_CLASS_DEF("URIError", &js_uri_error_class),
    JS_PROP_CLASS_DEF("InternalError", &js_internal_error_class),
    JS_PROP_CLASS_DEF("ArrayBuffer", &js_array_buffer_class),
    JS_PROP_CLASS_DEF("Uint8ClampedArray", &js_Uint8ClampedArray_class),
    JS_PROP_CLASS_DEF("Int8Array", &js_Int8Array_class),
    JS_PROP_CLASS_DEF("Uint8Array", &js_Uint8Array_class),
    JS_PROP_CLASS_DEF("Int16Array", &js_Int16Array_class),
    JS_PROP_CLASS_DEF("Uint16Array", &js_Uint16Array_class),
    JS_PROP_CLASS_DEF("Int32Array", &js_Int32Array_class),
    JS_PROP_CLASS_DEF("Uint32Array", &js_Uint32Array_class),
    JS_PROP_CLASS_DEF("Float32Array", &js_Float32Array_class),
    JS_PROP_CLASS_DEF("Float64Array", &js_Float64Array_class),
    JS_PROP_DOUBLE_DEF("Infinity", 1.0 / 0.0, 0),
    JS_PROP_DOUBLE_DEF("NaN", NAN, 0),
    JS_PROP_UNDEFINED_DEF("undefined", 0),
    JS_PROP_NULL_DEF("globalThis", 0),
    /* Bare top-level parseInt/parseFloat/isNaN/isFinite -- real scripts use
     * these ES5-style, not as Number.parseInt/etc (App-Store.js found
     * calling bare `parseInt` 2026-09-17). Same real functions already
     * registered on Number above/js_global_isNaN|isFinite from
     * mquickjs.c -- just also exposed unqualified, matching mqjs_stdlib.c's
     * own js_global_object[] which registers both forms. */
    JS_CFUNC_DEF("parseInt", 2, js_number_parseInt),
    JS_CFUNC_DEF("parseFloat", 1, js_number_parseFloat),
    JS_CFUNC_DEF("isNaN", 1, js_global_isNaN),
    JS_CFUNC_DEF("isFinite", 1, js_global_isFinite),

    /* Bruce-specific namespaces. */
    JS_PROP_CLASS_DEF("subghz", &js_subghz_obj),
    JS_PROP_CLASS_DEF("gpio", &js_gpio_obj),
    JS_PROP_CLASS_DEF("device", &js_device_obj),
    JS_PROP_CLASS_DEF("storage", &js_storage_obj),
    JS_PROP_CLASS_DEF("keyboard", &js_keyboard_obj),
    JS_PROP_CLASS_DEF("display", &js_display_obj),
    JS_PROP_CLASS_DEF("audio", &js_audio_obj),
    JS_PROP_CLASS_DEF("wifi", &js_wifi_obj),
    JS_PROP_CLASS_DEF("Math", &js_math_obj),
    JS_PROP_CLASS_DEF("JSON", &js_json_obj),
    JS_PROP_CLASS_DEF("console", &js_console_obj),
    /* Bare top-level functions, matching Bruce's own placement (not
     * namespaced under any object) -- see globals_js.cpp's native_now/
     * native_delay/native_random. */
    JS_CFUNC_DEF("now", 0, native_now),
    JS_CFUNC_DEF("delay", 1, native_delay),
    JS_CFUNC_DEF("random", 2, native_random),
    /* require(name) -- real Bruce scripts load every namespace this way
     * ("var display = require('display');") rather than touching the bare
     * global directly; see native_require()'s own comment in bruce_js_app.c
     * for why this was the actual root cause of "Script: Unknown" on every
     * real script tested, not a display/keyboard/audio binding gap. */
    JS_CFUNC_DEF("require", 1, native_require),
    /* gc() -- real scripts (App-Store.js included) call this directly; see
     * native_gc()'s own comment in bruce_js_app.c for why a no-op is safe. */
    JS_CFUNC_DEF("gc", 0, native_gc),
    JS_PROP_END
};

static const JSPropDef c_function_decl[] = {
    JS_PROP_END
};

int main(int argc, char **argv) {
    return build_atoms("bruce_stdlib", global_object, c_function_decl, argc, argv);
}
