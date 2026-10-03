idf_component_register(
    SRCS
        "../../lib/mquickjs/mquickjs.c"
        "../../lib/mquickjs/cutils.c"
        "../../lib/mquickjs/dtoa.c"
        "../../lib/mquickjs/libm.c"
    INCLUDE_DIRS
        "../../lib/mquickjs"
        "../../applications/system/bruce_js_app/gen"
    REQUIRES furi furi_hal toolbox mlib storage
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-function-declaration
    -Wno-error=incompatible-pointer-types
    -Wno-unused-function
    -Wno-unused-variable
    # Vendored code was written assuming uint32_t == unsigned int (true on the
    # x86/ARM hosts it targets); on this Xtensa/newlib toolchain uint32_t is
    # `long unsigned int`, so its own correct %u/%x/%d format strings trip
    # -Wformat. Not a real bug in the vendored source, just a target-specific
    # typedef mismatch -- downgrade to non-fatal rather than hand-patch an
    # 18k-line vendored file.
    -Wno-error=format
    # mquickjs's built-in Math implementation happens to name its internal
    # (non-static) helpers identically to this port's own, unrelated
    # applications/system/js_app/modules/js_math.c (mjs's math bindings for
    # the OTHER JS engine already in this firmware) -- js_math_sign/clz32/
    # atan2/pow/random collide at link time. Rename mquickjs's copies via
    # preprocessor token substitution rather than editing either vendored/
    # existing source file.
    -Djs_math_sign=mquickjs_js_math_sign
    -Djs_math_clz32=mquickjs_js_math_clz32
    -Djs_math_atan2=mquickjs_js_math_atan2
    -Djs_math_pow=mquickjs_js_math_pow
    -Djs_math_random=mquickjs_js_math_random
)
