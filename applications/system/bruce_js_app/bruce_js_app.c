/* Bruce JS: runs Bruce-firmware-compatible .bjs/.js scripts on the vendored
 * mquickjs engine (a different, incompatible engine from this port's own
 * mjs-based js_app). See bruce_js_app() for the file-loading/menu-launch
 * entry point and gen/bruce_stdlib_gen.c for the native binding table. */
#include <furi.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <gui/gui.h>
#include <input/input.h>

#include <toolbox/cli/cli_command.h>
#include <cli/cli_main_commands.h>
#include <toolbox/pipe.h>

#include <lib/subghz/devices/devices.h>
#include "js_app/modules/js_subghz/radio_device_loader.h"
#include <furi_hal_gpio.h>
#include <furi/core/memmgr.h>

#include <storage/storage.h>
#include <toolbox/path.h>
#include <furi_hal_display.h>
#include <esp_heap_caps.h>
#include <wifi.h>
#include <wlan_hal.h>
#include <wlan_passwords.h>
#include <esp_http_client.h>

#include "mquickjs.h"
#include "bruce_js_font5x7.h"

/* js_json_parse() is a real internal mquickjs engine function (Phase 3.5
 * found the same "compiled in, never exposed" situation for Math/typed
 * arrays -- see gen/bruce_stdlib_gen.c's js_json[] table). Its real
 * declaration only becomes visible transitively via "bruce_stdlib.h"
 * (generated, #includes mquickjs_priv.h) -- but that include has to come
 * AFTER every native_* function in this file (the generated table references
 * them by name), so native_wifi_http_fetch() below, which calls
 * js_json_parse() directly to implement responseType:"json", needs this
 * forward declaration to see it first. Matches mquickjs_priv.h's real
 * prototype exactly. */
extern JSValue js_json_parse(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv);

#define TAG "BruceJs"

/* Real Bruce scripts run bigger than the original 64K guess -- the OFFICIAL
 * App Store.js alone is ~40KB of SOURCE, and mquickjs needs arena room for
 * both the compiled bytecode AND the runtime heap on top of that, so 64K was
 * silently rejecting real scripts ("JS limit prevents launch what I pick").
 * This board has 8MB PSRAM (~4.5MB already in the heap allocator pool per
 * boot log), so there's no reason to size this tight against internal DRAM --
 * allocate from PSRAM like the RGB framebuffer already does, matching
 * wolf3d/sdl_glue.cpp's heap_caps_malloc(..., MALLOC_CAP_SPIRAM) convention,
 * rather than plain malloc() competing with WiFi/BLE for scarce internal
 * DRAM (see wifi-ble-memory-fix-failed/evil-portal-rx-starvation memories). */
#define BRUCE_JS_ARENA_SIZE (256 * 1024)

/* Bruce scripts' own folder. Matches the real Bruce App Store's own hardcoded
 * install path, "/BruceJS/[category]/" (capital JS, see App-Store-Data's
 * publishing spec) -- NOT the port's own earlier "BruceJs" casing -- so a
 * script the App Store installs ends up somewhere this app's own file
 * browser (and Archive/.bjs dispatch) actually looks. */
#define BRUCE_JS_SCRIPTS_FOLDER EXT_PATH("BruceJS")
/* Comfortably above the official App Store.js (~40KB source) with headroom
 * for bigger real-world scripts; must stay well under BRUCE_JS_ARENA_SIZE
 * since the arena also has to hold compiled bytecode + runtime heap on top
 * of the raw source text. */
#define BRUCE_JS_MAX_SCRIPT_SIZE (128 * 1024)

/* Hard cap on a single httpFetch() response body -- PSRAM-backed like the
 * script arena/framebuffer, but still bounded so a broken/huge/malicious
 * server response can't run this board out of PSRAM from inside a script. */
#define BRUCE_JS_HTTP_MAX_RESPONSE (512 * 1024)

/* Real Bruce App Store bootstrap target -- see Bruce-Firmware/src/core/
 * settings.cpp's installAppStoreJS()/appStoreInstalled(): a plain HTTP GET
 * whose raw response body IS the catalog-browser script, written verbatim to
 * this exact path (capital "Tools", matching that function's own
 * mkdir("/BruceJS/Tools")) -- no JSON catalog fetched natively; the catalog
 * UI lives entirely inside this downloaded .js, run through this port's own
 * engine like any other script once fetched. */
/* HTTP, not HTTPS: hardware-confirmed 2026-09-18 that this board's mbedtls
 * fails the TLS handshake against this exact host (Cloudflare-fronted) --
 * "mbedtls_ssl_handshake returned -0x7780" (MBEDTLS_ERR_SSL_FATAL_ALERT_
 * MESSAGE, i.e. the SERVER terminates the handshake), and an unrelated
 * pre-existing HTTPS caller (WlanHal's timezone lookup, to a different host)
 * failed the exact same way moments earlier -- this looks like a systemic
 * on-device TLS issue, not anything specific to this URL or code, and
 * nothing in this codebase has a working HTTPS track record to fall back on
 * (native_wifi_http_fetch's own comment already notes the real App Store's
 * data host is plain HTTP for this reason). Verified directly from a normal
 * client (not just ESP32) that this URL serves IDENTICAL content over plain
 * HTTP with no redirect (same Content-Length, same body) -- so this sidesteps
 * the on-device TLS problem entirely rather than trying to fix it blind. */
#define BRUCE_JS_APPSTORE_URL "http://ghp.iceis.co.uk/service/appstore/"
#define BRUCE_JS_APPSTORE_TOOLS_DIR EXT_PATH("BruceJS/Tools")
#define BRUCE_JS_APPSTORE_SCRIPT_PATH BRUCE_JS_APPSTORE_TOOLS_DIR "/App Store.js"

/* Raw RGB565 framebuffer primitives -- moved up here (ahead of every
 * native_* binding) because the display.* bindings below call these
 * directly, and C needs them declared/defined before first use. Originally
 * lived next to the higher-level screen-drawing code further down
 * (bruce_js_draw_header()/_screen()/_menu()); those can stay where they are
 * since nothing calls them before their own definition. */
static inline uint16_t bruce_js_swap16(uint16_t v) {
    return (uint16_t)(((v & 0xFF) << 8) | (v >> 8));
}

static inline uint16_t bruce_js_pack_swap(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
    return bruce_js_swap16(v);
}

/* Bruce's own real default accent (Bruce-Firmware/src/core/theme.h:
 * DEFAULT_PRICOLOR 0xA80F, decoded from RGB565 to RGB888 -> ~0xAC, 0x00,
 * 0x7B) -- used instead of an invented color so this "stripped" UI actually
 * looks like official Bruce, not just a same-shaped clone in a different
 * palette. */
static inline uint16_t bruce_js_accent_color(void) {
    return bruce_js_pack_swap(0xac, 0x00, 0x7b);
}

static void bruce_js_fb_fill(uint16_t* fb, int w, int h, uint16_t color) {
    for(int i = 0; i < w * h; i++) fb[i] = color;
}

static void
    bruce_js_fb_fill_rect(uint16_t* fb, int w, int h, int x0, int y0, int rw, int rh, uint16_t color) {
    for(int y = y0 < 0 ? 0 : y0; y < y0 + rh && y < h; y++) {
        for(int x = x0 < 0 ? 0 : x0; x < x0 + rw && x < w; x++) {
            fb[y * w + x] = color;
        }
    }
}

/* Outline-only rectangle. */
static void
    bruce_js_fb_draw_rect(uint16_t* fb, int w, int h, int x0, int y0, int rw, int rh, uint16_t color) {
    bruce_js_fb_fill_rect(fb, w, h, x0, y0, rw, 1, color);
    bruce_js_fb_fill_rect(fb, w, h, x0, y0 + rh - 1, rw, 1, color);
    bruce_js_fb_fill_rect(fb, w, h, x0, y0, 1, rh, color);
    bruce_js_fb_fill_rect(fb, w, h, x0 + rw - 1, y0, 1, rh, color);
}

/* Filled rounded rectangle -- a per-pixel quarter-circle clip at each corner,
 * matching the RADIUS Bruce's own display.cpp uses (fillRoundRect(...,5,...)
 * everywhere: drawOptions()'s box, drawStatusBar()'s border). Not a TFT_eSPI
 * port, just close enough geometry that it reads as the same shape. */
static void bruce_js_fb_fill_round_rect(
    uint16_t* fb,
    int w,
    int h,
    int x0,
    int y0,
    int rw,
    int rh,
    int radius,
    uint16_t color) {
    if(radius > rw / 2) radius = rw / 2;
    if(radius > rh / 2) radius = rh / 2;
    for(int y = 0; y < rh; y++) {
        for(int x = 0; x < rw; x++) {
            int px = x0 + x, py = y0 + y;
            if(px < 0 || px >= w || py < 0 || py >= h) continue;
            int cx = -1, cy = -1;
            if(x < radius && y < radius) {
                cx = radius;
                cy = radius;
            } else if(x >= rw - radius && y < radius) {
                cx = rw - radius - 1;
                cy = radius;
            } else if(x < radius && y >= rh - radius) {
                cx = radius;
                cy = rh - radius - 1;
            } else if(x >= rw - radius && y >= rh - radius) {
                cx = rw - radius - 1;
                cy = rh - radius - 1;
            }
            if(cx >= 0) {
                int dx = x - cx, dy = y - cy;
                if(dx * dx + dy * dy > radius * radius) continue;
            }
            fb[py * w + px] = color;
        }
    }
}

/* Outline rounded rectangle: fill big, then fill a smaller inset rounded
 * rect in the background color on top -- a cheap but correct way to get a
 * rounded BORDER without a real stroke rasterizer. */
static void bruce_js_fb_draw_round_rect(
    uint16_t* fb,
    int w,
    int h,
    int x0,
    int y0,
    int rw,
    int rh,
    int radius,
    uint16_t color,
    uint16_t bg_color) {
    bruce_js_fb_fill_round_rect(fb, w, h, x0, y0, rw, rh, radius, color);
    int inset = 2;
    int inner_radius = radius > inset ? radius - inset : 0;
    bruce_js_fb_fill_round_rect(
        fb, w, h, x0 + inset, y0 + inset, rw - 2 * inset, rh - 2 * inset, inner_radius, bg_color);
}

static void bruce_js_fb_draw_char(
    uint16_t* fb,
    int w,
    int h,
    int x,
    int y,
    char c,
    uint16_t color,
    int scale) {
    if(c < 32 || c > 126) c = '?';
    const uint8_t* glyph = bruce_font_5x7[(uint8_t)c - 32];
    for(int gx = 0; gx < BRUCE_FONT_CHAR_W; gx++) {
        uint8_t col = glyph[gx];
        for(int gy = 0; gy < BRUCE_FONT_CHAR_H; gy++) {
            if(!(col & (1 << gy))) continue;
            for(int sx = 0; sx < scale; sx++) {
                for(int sy = 0; sy < scale; sy++) {
                    int px = x + gx * scale + sx;
                    int py = y + gy * scale + sy;
                    if(px >= 0 && px < w && py >= 0 && py < h) fb[py * w + px] = color;
                }
            }
        }
    }
}

static void bruce_js_fb_draw_text(
    uint16_t* fb,
    int w,
    int h,
    int x,
    int y,
    const char* text,
    uint16_t color,
    int scale) {
    int cx = x;
    for(const char* p = text; *p; p++) {
        bruce_js_fb_draw_char(fb, w, h, cx, y, *p, color, scale);
        cx += (BRUCE_FONT_CHAR_W + 1) * scale;
    }
}

/* Wraps at a fixed column count (no word-boundary logic -- this is a status
 * screen, not a text editor) and treats '\n' in the source string as an
 * explicit break, since JS_ToCString() output (e.g. a thrown error message)
 * can contain real newlines. */
static void bruce_js_fb_draw_wrapped(
    uint16_t* fb,
    int w,
    int h,
    int x,
    int y,
    int max_width,
    int line_height,
    const char* text,
    uint16_t color,
    int scale) {
    int cols = max_width / ((BRUCE_FONT_CHAR_W + 1) * scale);
    if(cols < 1) cols = 1;
    int row = 0, col = 0;
    int cx = x, cy = y;
    for(const char* p = text; *p; p++) {
        if(*p == '\n' || col >= cols) {
            row++;
            col = 0;
            cx = x;
            cy = y + row * line_height;
            if(*p == '\n') continue;
        }
        bruce_js_fb_draw_char(fb, w, h, cx, cy, *p, color, scale);
        cx += (BRUCE_FONT_CHAR_W + 1) * scale;
        col++;
    }
}

/* subghz.isFrequencyValid(freq) -- Phase 2's first real binding. Declared
 * here (native side) before #include "bruce_stdlib.h" below, since the
 * generated table's tail literally initializes a function pointer to this
 * name (see gen/bruce_stdlib_gen.c for the table this implements). Reuses
 * the same radio_device_loader.{h,c} already linked into this firmware for
 * js_subghz (the OTHER JS engine's SubGHz module) -- not recompiled here,
 * just called, so this can never collide the way js_math did in Phase 1.
 *
 * ROOT CAUSE of the crash this session hit twice before finding this: the
 * global SubGHz device registry needs subghz_devices_init() called before
 * ANY subghz_devices_*()/radio_device_loader_*() use, and subghz_devices_
 * deinit() after -- js_subghz_create()/_destroy() in js_subghz.c do exactly
 * this, paired to the module's lifetime. This function never called init()
 * at all, so radio_device_loader_set() below hit an unmet furi_check() in
 * the registry every single time, regardless of which thread/context called
 * it (STARTUP hook, resume-app, or a genuine manual menu tap -- all three
 * were tried and all three crashed identically, which in hindsight should
 * have been the tell that it wasn't a context/timing issue at all). Calling
 * init()/deinit() as a tightly-scoped pair around just this one call is safe
 * as long as no other app (ProtoPirate, js_subghz) has the registry open at
 * the same moment -- true under this port's single-foreground-app model. */
static JSValue native_subghz_is_frequency_valid(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    uint32_t freq = 0;
    if(argc >= 1) {
        JS_ToUint32(ctx, &freq, argv[0]);
    }

    subghz_devices_init();
    const SubGhzDevice* device = radio_device_loader_set(NULL, SubGhzRadioDeviceTypeExternalCC1101);
    bool valid = device ? subghz_devices_is_frequency_valid(device, freq) : false;
    if(device) {
        radio_device_loader_end(device);
    }
    subghz_devices_deinit();
    return JS_NewBool(valid);
}

/* gpio.pinMode/digitalWrite/digitalRead -- Phase 3's first "hard namespace"
 * binding. GpioPin on this ESP32 port is just {port=NULL, pin=N}, so any
 * integer maps directly to a pin (see components/furi_hal/furi_hal_gpio.h).
 * These do not validate the pin against pins already used by LCD/SD/radio/
 * etc. -- same as Bruce's own gpio_js.cpp, which calls Arduino's bare
 * digitalWrite/pinMode with no such check either. Reachable both from the
 * brucejs CLI's free-form expression argument and from any real .bjs/.js
 * script loaded via bruce_js_app() (menu launch or Archive/Loader dispatch)
 * -- the pin-conflict risk is inherent to what the script asks for, not to
 * which path invoked it. */
static JSValue native_gpio_pin_mode(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    uint32_t pin = 0, mode = 0;
    if(argc >= 2) {
        JS_ToUint32(ctx, &pin, argv[0]);
        JS_ToUint32(ctx, &mode, argv[1]);
    }
    const GpioPin gpio = {.port = NULL, .pin = (uint16_t)pin};
    furi_hal_gpio_init_simple(&gpio, mode ? GpioModeOutputPushPull : GpioModeInput);
    return JS_UNDEFINED;
}

static JSValue native_gpio_digital_write(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    uint32_t pin = 0, value = 0;
    if(argc >= 2) {
        JS_ToUint32(ctx, &pin, argv[0]);
        JS_ToUint32(ctx, &value, argv[1]);
    }
    const GpioPin gpio = {.port = NULL, .pin = (uint16_t)pin};
    furi_hal_gpio_write(&gpio, value != 0);
    return JS_UNDEFINED;
}

static JSValue native_gpio_digital_read(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    uint32_t pin = 0;
    if(argc >= 1) {
        JS_ToUint32(ctx, &pin, argv[0]);
    }
    const GpioPin gpio = {.port = NULL, .pin = (uint16_t)pin};
    return JS_NewBool(furi_hal_gpio_read(&gpio));
}

/* device.getFreeHeap()/getUptime() -- pure info getters, zero hardware risk,
 * safe to eventually call from anywhere (unlike subghz/gpio above), kept
 * CLI-only for now just for consistency with the rest of this file. */
static JSValue native_device_get_free_heap(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    return JS_NewInt32(ctx, (int32_t)memmgr_get_free_heap());
}

static JSValue native_device_get_uptime(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    return JS_NewInt32(ctx, (int32_t)furi_get_tick());
}

/* device.getBoard()/getModel() -- lets a real Bruce script (or a future
 * Bruce App Store) detect exactly which hardware it's running on, matching
 * Bruce-Firmware's own DEVICE_NAME string for this board
 * (boards/lilygo-t-embed-cc1101/lilygo-t-embed-cc1101.ini:
 * -DDEVICE_NAME='"Lilygo T-Embed CC1101"') so app-store-style board checks
 * written against real Bruce firmware work unmodified here too. This port
 * only ever targets this one board (main/CMakeLists.txt refuses any other
 * FLIPPER_BOARD), so the string is hardcoded rather than derived. */
static JSValue native_device_get_board(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    return JS_NewString(ctx, "Lilygo T-Embed CC1101");
}

/* Shared state for one running script's keyboard/display calls. Lives
 * only for the duration of a single bruce_js_run_test() call (set up right
 * before JS_Eval, torn down right after) -- fine since only one script ever
 * runs at a time in this port's single-foreground-app model, so a static
 * struct needs no per-context plumbing through the JS bindings (which only
 * ever receive (ctx, this_val, argc, argv), no user-data pointer). */
typedef struct {
    bool gui_active; /* true only while bruce_js_app()'s own direct-draw
                       * session is up (menu/browser/result screens) -- false
                       * for a script run from the "brucejs" CLI command,
                       * which has no GUI/input context at all. The keyboard
                       * and display bindings silently no-op rather than
                       * touch hardware when this is false, instead of
                       * crashing. */
    FuriPubSub* input_events;
    FuriMessageQueue* input_queue;
    FuriPubSubSubscription* input_sub;
    bool esc, next, prev, sel, any;

    uint16_t* fb;
    int fb_w, fb_h;
    uint16_t text_color;
    int text_scale;
    int text_align_h; /* 0=left, 1=center, 2=right -- matches Bruce's own
                        * TFT_eSPI datum convention closely enough */
    int text_align_v; /* 0=top, 1=middle, 2=bottom, 3=alphabetic (treated as
                        * bottom -- this port's 5x7 font has no descenders to
                        * account for) -- same TFT_eSPI convention. */
} BruceJsScriptCtx;

static BruceJsScriptCtx g_script_ctx;

static void bruce_js_script_input_callback(const void* value, void* context) {
    FuriMessageQueue* queue = context;
    furi_message_queue_put(queue, value, FuriWaitForever);
}

/* Drains whatever input arrived since the last poll into the edge flags --
 * called at the top of every keyboard.getXPress() so a script's own tight
 * while(true) loop sees presses as they happen without blocking. */
static void bruce_js_script_poll_input(void) {
    if(!g_script_ctx.gui_active) return;
    InputEvent event;
    while(furi_message_queue_get(g_script_ctx.input_queue, &event, 0) == FuriStatusOk) {
        if(event.type != InputTypeShort) continue;
        g_script_ctx.any = true;
        switch(event.key) {
        case InputKeyBack:
            g_script_ctx.esc = true;
            break;
        case InputKeyDown:
            g_script_ctx.next = true;
            break;
        case InputKeyUp:
            g_script_ctx.prev = true;
            break;
        case InputKeyOk:
            g_script_ctx.sel = true;
            break;
        default:
            break;
        }
    }
}

/* keyboard.* -- edge-triggered (matches Bruce's own real semantics: a
 * getXPress() call reports a press that happened since the last call, not
 * "is currently held"). setLongPress() is accepted but not implemented --
 * this port's simple edge-tracking doesn't currently distinguish press
 * duration; a script that calls it just gets a silent no-op rather than a
 * crash, same simplification philosophy as audio.tone() below. */
static JSValue
    native_keyboard_set_long_press(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(ctx);
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    return JS_UNDEFINED;
}

static JSValue
    native_keyboard_get_prev_press(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    bruce_js_script_poll_input();
    bool v = g_script_ctx.prev;
    g_script_ctx.prev = false;
    return JS_NewBool(v);
}

static JSValue
    native_keyboard_get_next_press(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    bruce_js_script_poll_input();
    bool v = g_script_ctx.next;
    g_script_ctx.next = false;
    return JS_NewBool(v);
}

static JSValue
    native_keyboard_get_sel_press(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    bruce_js_script_poll_input();
    bool v = g_script_ctx.sel;
    g_script_ctx.sel = false;
    return JS_NewBool(v);
}

static JSValue
    native_keyboard_get_esc_press(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    bruce_js_script_poll_input();
    bool v = g_script_ctx.esc;
    g_script_ctx.esc = false;
    return JS_NewBool(v);
}

static JSValue
    native_keyboard_get_any_press(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    bruce_js_script_poll_input();
    bool v = g_script_ctx.any;
    g_script_ctx.any = false;
    return JS_NewBool(v);
}

/* display.* -- draws into the script-owned RGB565 framebuffer set up by
 * bruce_js_run_test() before JS_Eval; see gen/bruce_stdlib_gen.c's comment
 * on js_display[] for the createSprite()-as-alias simplification. */
static JSValue native_display_color(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    uint32_t r = 0, g = 0, b = 0;
    if(argc >= 3) {
        JS_ToUint32(ctx, &r, argv[0]);
        JS_ToUint32(ctx, &g, argv[1]);
        JS_ToUint32(ctx, &b, argv[2]);
    }
    return JS_NewInt32(ctx, (int32_t)bruce_js_pack_swap((uint8_t)r, (uint8_t)g, (uint8_t)b));
}

static JSValue native_display_width(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    return JS_NewInt32(ctx, g_script_ctx.gui_active ? g_script_ctx.fb_w : 0);
}

static JSValue native_display_height(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    return JS_NewInt32(ctx, g_script_ctx.gui_active ? g_script_ctx.fb_h : 0);
}

static JSValue
    native_display_set_text_color(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    uint32_t color = 0;
    if(argc >= 1) JS_ToUint32(ctx, &color, argv[0]);
    g_script_ctx.text_color = (uint16_t)color;
    return JS_UNDEFINED;
}

static JSValue
    native_display_set_text_size(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    uint32_t size = 1;
    if(argc >= 1) JS_ToUint32(ctx, &size, argv[0]);
    if(size < 1) size = 1;
    if(size > 4) size = 4;
    g_script_ctx.text_scale = (int)size;
    return JS_UNDEFINED;
}

/* Real Bruce's own native_setTextAlign() (display_js.cpp) accepts EITHER a
 * string (checked by its first letter: 'l'/'c'/'r' horizontal, 't'/'m'/'b'/
 * 'a' vertical) OR a number, then combines them into a TFT_eSPI textdatum
 * (align + baseline*3). The real App-Store.js always calls this with STRINGS
 * ("center","bottom" etc) -- this port's version used to call JS_ToUint32()
 * unconditionally, which for a non-numeric string silently coerces to 0 (per
 * ECMAScript ToNumber("center") -> NaN -> ToUint32(NaN) -> 0, no exception),
 * so every alignment request silently became "top-left" no matter what the
 * script actually asked for. Found 2026-09-18 running the real App-Store.js:
 * its whole UI (title, category names, status text) render top-left-anchored
 * at coordinates computed assuming center/middle/bottom anchoring, which
 * bunches everything toward one corner instead of spreading across the
 * screen. Fixed to parse strings the same way real Bruce does, with numeric
 * input still accepted as a fallback for any caller that uses it directly. */
static int bruce_js_parse_text_align_h(JSContext* ctx, JSValue arg) {
    if(JS_IsString(ctx, arg)) {
        JSCStringBuf sb;
        const char* s = JS_ToCString(ctx, arg, &sb);
        if(s && s[0] == 'c') return 1;
        if(s && s[0] == 'r') return 2;
        return 0; /* 'l' or anything else -> left, matching real Bruce */
    }
    uint32_t h = 0;
    JS_ToUint32(ctx, &h, arg);
    return (int)h;
}

static int bruce_js_parse_text_align_v(JSContext* ctx, JSValue arg) {
    if(JS_IsString(ctx, arg)) {
        JSCStringBuf sb;
        const char* s = JS_ToCString(ctx, arg, &sb);
        if(s && s[0] == 'm') return 1;
        if(s && s[0] == 'b') return 2;
        if(s && s[0] == 'a') return 3;
        return 0; /* 't' or anything else -> top, matching real Bruce */
    }
    uint32_t v = 0;
    JS_ToUint32(ctx, &v, arg);
    return (int)v;
}

static JSValue
    native_display_set_text_align(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    g_script_ctx.text_align_h = argc >= 1 ? bruce_js_parse_text_align_h(ctx, argv[0]) : 0;
    g_script_ctx.text_align_v = argc >= 2 ? bruce_js_parse_text_align_v(ctx, argv[1]) : 0;
    return JS_UNDEFINED;
}

/* Argument order bug found 2026-09-18 via temporary coordinate logging:
 * real Bruce's own native_drawString(text, x, y) (display_js.cpp) takes the
 * TEXT FIRST, then x, then y -- matching TFT_eSPI's drawString(s,x,y). This
 * port's version had them as (x, y, text) instead. Since the script always
 * passes a string as its first arg, JS_ToUint32() on that string silently
 * coerced to 0 (ToNumber("some text") -> NaN -> ToUint32 -> 0, no throw) --
 * explaining why x was always exactly 0. The script's actual x argument
 * (arg[1], typically Se/2=160 for centered calls) got read into `y` instead
 * -- explaining why y was always 160. And the script's actual y argument
 * (arg[2], one of F()'s computed "G"-position values like 24/81.25/116.75)
 * got read as the TEXT to render, coerced to its decimal string form -- this
 * is exactly the "text" this port was logging/drawing instead of the real
 * label. Every symptom (text crammed in one spot, garbage-looking numeric
 * strings, the "worse" result after fixing alignment -- correct alignment
 * math applied to already-scrambled inputs just moved the garbage further
 * off-screen) traces back to this one argument-order mismatch. */
static JSValue native_display_draw_text(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    if(!g_script_ctx.gui_active || argc < 3) return JS_UNDEFINED;
    JSCStringBuf buf;
    const char* text = JS_ToCString(ctx, argv[0], &buf);
    uint32_t x = 0, y = 0;
    JS_ToUint32(ctx, &x, argv[1]);
    JS_ToUint32(ctx, &y, argv[2]);
    if(!text) return JS_UNDEFINED;

    int scale = g_script_ctx.text_scale > 0 ? g_script_ctx.text_scale : 1;
    int text_w = (int)strlen(text) * (BRUCE_FONT_CHAR_W + 1) * scale;
    int text_h = BRUCE_FONT_CHAR_H * scale;
    int draw_x = (int)x;
    if(g_script_ctx.text_align_h == 1) {
        draw_x -= text_w / 2;
    } else if(g_script_ctx.text_align_h == 2) {
        draw_x -= text_w;
    }
    int draw_y = (int)y;
    if(g_script_ctx.text_align_v == 1) {
        draw_y -= text_h / 2;
    } else if(g_script_ctx.text_align_v >= 2) {
        draw_y -= text_h;
    }
    bruce_js_fb_draw_text(
        g_script_ctx.fb,
        g_script_ctx.fb_w,
        g_script_ctx.fb_h,
        draw_x,
        draw_y,
        text,
        g_script_ctx.text_color,
        scale);
    return JS_UNDEFINED;
}

static JSValue native_display_draw_rect(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    if(!g_script_ctx.gui_active || argc < 5) return JS_UNDEFINED;
    uint32_t x = 0, y = 0, w = 0, h = 0, color = 0;
    JS_ToUint32(ctx, &x, argv[0]);
    JS_ToUint32(ctx, &y, argv[1]);
    JS_ToUint32(ctx, &w, argv[2]);
    JS_ToUint32(ctx, &h, argv[3]);
    JS_ToUint32(ctx, &color, argv[4]);
    bruce_js_fb_draw_rect(
        g_script_ctx.fb, g_script_ctx.fb_w, g_script_ctx.fb_h, (int)x, (int)y, (int)w, (int)h, (uint16_t)color);
    return JS_UNDEFINED;
}

static JSValue
    native_display_draw_fill_rect(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    if(!g_script_ctx.gui_active || argc < 5) return JS_UNDEFINED;
    uint32_t x = 0, y = 0, w = 0, h = 0, color = 0;
    JS_ToUint32(ctx, &x, argv[0]);
    JS_ToUint32(ctx, &y, argv[1]);
    JS_ToUint32(ctx, &w, argv[2]);
    JS_ToUint32(ctx, &h, argv[3]);
    JS_ToUint32(ctx, &color, argv[4]);
    bruce_js_fb_fill_rect(
        g_script_ctx.fb, g_script_ctx.fb_w, g_script_ctx.fb_h, (int)x, (int)y, (int)w, (int)h, (uint16_t)color);
    return JS_UNDEFINED;
}

/* display.fill(color) / sprite.fill(color) -- whole-surface solid fill,
 * matching real Bruce's Sprite::fill() convenience method (missed in the
 * initial display.* pass since the earlier structural analysis of
 * dino_game.js's own API surface didn't surface it -- found instead via the
 * host-side bisection, another real script call this port never had). */
static JSValue native_display_fill(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    if(!g_script_ctx.gui_active || argc < 1) return JS_UNDEFINED;
    uint32_t color = 0;
    JS_ToUint32(ctx, &color, argv[0]);
    bruce_js_fb_fill(g_script_ctx.fb, g_script_ctx.fb_w, g_script_ctx.fb_h, (uint16_t)color);
    return JS_UNDEFINED;
}

/* display.drawXBitmap(x, y, bitmapData, w, h, color) -- another real call
 * this port never had (found the same way as .fill()). Generic 1bpp XBM
 * blit: `bitmapData` is a Uint8Array, row-major, LSB-first within each byte
 * (standard XBM packing -- matches the "This is XBM format" comment real
 * Bruce scripts embed above their own sprite data). This function only
 * reads whatever bytes the CALLER supplies and sets matching pixels in the
 * given color; it has no knowledge of what image the bytes represent. */
static JSValue
    native_display_draw_xbitmap(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    if(!g_script_ctx.gui_active || argc < 6) return JS_UNDEFINED;
    uint32_t x = 0, y = 0, bmp_w = 0, bmp_h = 0, color = 0;
    JS_ToUint32(ctx, &x, argv[0]);
    JS_ToUint32(ctx, &y, argv[1]);
    JS_ToUint32(ctx, &bmp_w, argv[3]);
    JS_ToUint32(ctx, &bmp_h, argv[4]);
    JS_ToUint32(ctx, &color, argv[5]);

    size_t data_len = 0;
    const uint8_t* data = (const uint8_t*)JS_GetTypedArrayBuffer(ctx, &data_len, argv[2]);
    if(!data) return JS_UNDEFINED;

    uint32_t row_bytes = (bmp_w + 7) / 8;
    for(uint32_t row = 0; row < bmp_h; row++) {
        for(uint32_t col = 0; col < bmp_w; col++) {
            size_t byte_idx = (size_t)row * row_bytes + col / 8;
            if(byte_idx >= data_len) continue;
            if(!((data[byte_idx] >> (col % 8)) & 1)) continue;
            int px = (int)x + (int)col;
            int py = (int)y + (int)row;
            if(px >= 0 && px < g_script_ctx.fb_w && py >= 0 && py < g_script_ctx.fb_h) {
                g_script_ctx.fb[py * g_script_ctx.fb_w + px] = (uint16_t)color;
            }
        }
    }
    return JS_UNDEFINED;
}

/* sprite.pushSprite() -- real Bruce's actual "show this off-screen sprite on
 * the real panel now" call (found the same way as .fill()/.drawXBitmap()).
 * delay() already blits the framebuffer once per frame (see its own
 * comment), but a script may call pushSprite() at a different point in its
 * loop than delay() -- blit here too so the frame shows up right when the
 * script asks for it, not just whenever delay() next runs. */
static JSValue
    native_display_push_sprite(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(ctx);
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    if(g_script_ctx.gui_active && g_script_ctx.fb) {
        furi_hal_display_blit_rgb565(
            0, 0, (uint16_t)g_script_ctx.fb_w, (uint16_t)g_script_ctx.fb_h, g_script_ctx.fb);
    }
    return JS_UNDEFINED;
}

static JSValue
    native_display_create_sprite(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    JSValue global = JS_GetGlobalObject(ctx);
    return JS_GetPropertyStr(ctx, global, "display");
}

/* require(name) -- THE actual root cause of "Script: Unknown" on every real
 * script tested (found via a host-side test harness, gen/bruce_js_host_test.c,
 * bisecting a real script line-by-line rather than continuing to guess):
 * real Bruce scripts load every namespace via "var display =
 * require('display');" at the top of the file, not by touching the bare
 * global directly -- this port never had a `require` function at all, so
 * line 1 of literally any real script threw a ReferenceError before
 * anything else ran. Every namespace this port implements (display,
 * keyboard, audio, subghz, gpio, device, storage) is already a real global
 * set up at context-creation time, so require() only needs to hand back
 * that same object by name -- no actual dynamic module loading, matching
 * how trivial "require shims" work in other embedded JS environments. */
static JSValue native_require(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    if(argc < 1) return JS_UNDEFINED;
    JSCStringBuf buf;
    const char* name = JS_ToCString(ctx, argv[0], &buf);
    if(!name) return JS_UNDEFINED;
    JSValue global = JS_GetGlobalObject(ctx);
    return JS_GetPropertyStr(ctx, global, name);
}

/* console.log(...) -- App-Store.js (and presumably most real scripts) uses
 * this heavily for debug output. This engine's real `js_print` (the backing
 * function mqjs_stdlib.c's own `console`/`print` registration would use)
 * lives in mqjs.c, the standalone REPL binary this component doesn't
 * compile -- same "REPL-only" situation as Date/performance. Hand-written
 * instead: joins every argument with a space (matching real console.log
 * semantics for multiple args) and routes to this app's own FURI_LOG_I, the
 * same log stream every other diagnostic in this file already uses. Capped
 * at a fixed line buffer -- a debug log line doesn't need to be unbounded,
 * and this matches the same style already used for bruce_js_run_test()'s
 * own result buffer. */
static JSValue native_console_log(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    char line[256];
    size_t pos = 0;
    for(int i = 0; i < argc && pos < sizeof(line) - 1; i++) {
        if(i > 0 && pos < sizeof(line) - 1) line[pos++] = ' ';
        JSCStringBuf buf;
        const char* s = JS_ToCString(ctx, argv[i], &buf);
        if(s) {
            size_t len = strlen(s);
            size_t room = sizeof(line) - 1 - pos;
            if(len > room) len = room;
            memcpy(line + pos, s, len);
            pos += len;
        }
    }
    line[pos] = '\0';
    FURI_LOG_I(TAG, "%s", line);
    return JS_UNDEFINED;
}

/* gc() -- App-Store.js calls this directly (real Bruce scripts sometimes
 * force a collection after freeing a big buffer). This stripped mquickjs
 * build has no exposed manual-GC entry point (checked: no `js_gc`-shaped
 * function in the vendored core, only the standalone REPL's own `js_gc` in
 * mqjs.c, which this component doesn't compile -- same "REPL-only, not in
 * the portable core" situation as console/Date/print). A no-op is safe: the
 * engine still does its own automatic memory management regardless; a
 * script calling gc() is just hinting, not relying on the call for
 * correctness. */
static JSValue native_gc(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(ctx);
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    return JS_UNDEFINED;
}

/* audio.tone(freq, durationMs) -- deliberate no-op, see gen/bruce_stdlib_gen.c's
 * comment: this board has no piezo buzzer, only a non-audio WS2812 LED ring. */
static JSValue native_audio_tone(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(ctx);
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    return JS_UNDEFINED;
}

static JSValue native_now(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    return JS_NewInt32(ctx, (int32_t)furi_get_tick());
}

/* delay(ms) -- also the natural once-per-frame point to blit the script's
 * framebuffer to the real panel (see js_display[]'s comment for why drawing
 * doesn't blit immediately). A script that never calls delay() never sees
 * its own draws on screen -- matches the fact that real Bruce scripts always
 * call delay() once per game-loop iteration. */
static JSValue native_delay(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    if(g_script_ctx.gui_active && g_script_ctx.fb) {
        furi_hal_display_blit_rgb565(
            0, 0, (uint16_t)g_script_ctx.fb_w, (uint16_t)g_script_ctx.fb_h, g_script_ctx.fb);
    }
    if(argc >= 1) {
        uint32_t ms = 0;
        JS_ToUint32(ctx, &ms, argv[0]);
        furi_delay_ms(ms);
    }
    return JS_UNDEFINED;
}

static JSValue native_random(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    int val;
    if(argc >= 2) {
        int a = 0, b = 0;
        JS_ToInt32(ctx, &a, argv[0]);
        JS_ToInt32(ctx, &b, argv[1]);
        val = b > a ? a + (rand() % (b - a)) : a;
    } else if(argc >= 1) {
        int a = 0;
        JS_ToInt32(ctx, &a, argv[0]);
        val = a > 0 ? rand() % a : 0;
    } else {
        val = rand();
    }
    return JS_NewInt32(ctx, val);
}

/* Math.* -- the SECOND actual root cause behind "Script: Unknown" (found via
 * the same host-side bisection as require()/Uint8Array, see gen/
 * bruce_js_host_test.c and the bruce-js-engine-port memory): this
 * deliberately stripped "Micro QuickJS" build only implements 6 Math
 * functions natively (min_max, imul, clz32, atan2, pow, random -- confirmed
 * by grepping the vendored mquickjs.c for every js_math_* symbol) and
 * doesn't even register a global `Math` object at all ("typeof Math" ->
 * "undefined" in this engine, confirmed the same way Uint8Array's absence
 * was). Bruce's own real script here only needs abs/floor/round; ceil/sqrt/
 * min/max are added too since they're equally trivial and any other real
 * script is likely to need at least one of them -- full Math.* parity
 * (trig functions, log, etc.) is NOT attempted, only what's cheap and
 * commonly needed. */
static JSValue native_math_abs(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    double d = 0;
    if(argc >= 1) JS_ToNumber(ctx, &d, argv[0]);
    return JS_NewFloat64(ctx, fabs(d));
}

static JSValue native_math_floor(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    double d = 0;
    if(argc >= 1) JS_ToNumber(ctx, &d, argv[0]);
    return JS_NewFloat64(ctx, floor(d));
}

static JSValue native_math_ceil(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    double d = 0;
    if(argc >= 1) JS_ToNumber(ctx, &d, argv[0]);
    return JS_NewFloat64(ctx, ceil(d));
}

static JSValue native_math_round(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    double d = 0;
    if(argc >= 1) JS_ToNumber(ctx, &d, argv[0]);
    return JS_NewFloat64(ctx, floor(d + 0.5));
}

static JSValue native_math_sqrt(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    double d = 0;
    if(argc >= 1) JS_ToNumber(ctx, &d, argv[0]);
    return JS_NewFloat64(ctx, sqrt(d));
}

/* Math.trunc/pow/sign/log/exp -- found missing 2026-09-17 the same way
 * Phase 3.5 found the require()/Uint8Array gaps: running a real script (the
 * official App-Store.js) through the host harness threw "TypeError: not a
 * function" at its very first Math call, `Math.trunc(...)` on line 40. This
 * hand-written Math table only ever covered what earlier test scripts
 * happened to call, not real Math usage in general -- adding these five
 * (cheap libm one-liners, same pattern as the rest of this table) since any
 * further real script is likely to hit one of them next otherwise. */
static JSValue native_math_trunc(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    double d = 0;
    if(argc >= 1) JS_ToNumber(ctx, &d, argv[0]);
    return JS_NewFloat64(ctx, trunc(d));
}

static JSValue native_math_pow(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    double base = 0, exp = 0;
    if(argc >= 1) JS_ToNumber(ctx, &base, argv[0]);
    if(argc >= 2) JS_ToNumber(ctx, &exp, argv[1]);
    return JS_NewFloat64(ctx, pow(base, exp));
}

static JSValue native_math_sign(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    double d = 0;
    if(argc >= 1) JS_ToNumber(ctx, &d, argv[0]);
    return JS_NewFloat64(ctx, d > 0 ? 1 : (d < 0 ? -1 : d));
}

static JSValue native_math_log(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    double d = 0;
    if(argc >= 1) JS_ToNumber(ctx, &d, argv[0]);
    return JS_NewFloat64(ctx, log(d));
}

static JSValue native_math_exp(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    double d = 0;
    if(argc >= 1) JS_ToNumber(ctx, &d, argv[0]);
    return JS_NewFloat64(ctx, exp(d));
}

static JSValue native_math_min(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    double result = INFINITY;
    for(int i = 0; i < argc; i++) {
        double d = 0;
        JS_ToNumber(ctx, &d, argv[i]);
        if(d < result) result = d;
    }
    return JS_NewFloat64(ctx, result);
}

static JSValue native_math_max(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    double result = -INFINITY;
    for(int i = 0; i < argc; i++) {
        double d = 0;
        JS_ToNumber(ctx, &d, argv[i]);
        if(d > result) result = d;
    }
    return JS_NewFloat64(ctx, result);
}

/* storage.read/write/remove/readdir -- ONLY the 4 methods the real, official
 * Bruce App Store script (github.com/BruceDevices/App-Store, "App Store.js")
 * actually calls, not Bruce's whole storage.* surface (that also has rename/
 * mkdir/rmdir/space-usage/binary-reads/positional-writes/dual LittleFS-vs-SD
 * selection -- unneeded until something else calls for it). This port has one
 * unified SD storage backend, so paths are resolved relative to its root
 * (EXT_PATH) unless already absolute under /ext or /int -- matching how real
 * Bruce scripts use paths relative to their own single SD root. */
static void bruce_js_resolve_path(const char* raw_path, FuriString* out) {
    if(strncmp(raw_path, "/ext", 4) == 0 || strncmp(raw_path, "/int", 4) == 0) {
        furi_string_set(out, raw_path);
    } else if(raw_path[0] == '/') {
        furi_string_printf(out, "%s%s", STORAGE_EXT_PATH_PREFIX, raw_path);
    } else {
        furi_string_printf(out, "%s/%s", STORAGE_EXT_PATH_PREFIX, raw_path);
    }
}

/* Found missing 2026-09-17 running the real App-Store.js: every one of its
 * storage.* calls passes an OBJECT first argument, "{ fs, path }" (e.g.
 * storage.read({ fs: "sd", path: "/bruce.conf" })), not a plain path string
 * -- this port's storage.* only ever accepted a bare string, so the very
 * first storage call in a real script would have thrown immediately (this
 * script's own file-system-detection code, ironically, is what calls it).
 * This port has one unified SD backend with no "fs" (LittleFS vs SD)
 * selection to make, so `.fs` is read and ignored -- same "accept and
 * ignore an option this port has no equivalent for" precedent already used
 * for wifi.httpFetch's `save.fs`. Still accepts a plain string too, for any
 * simpler caller (or CLI testing). */
/* Real Bruce's native_storageWrite() opens with FS::open(path, mode, true) --
 * that trailing `true` is Arduino's own ESP32 FS "create" flag, which
 * recursively creates every missing parent directory (confirmed by reading
 * Bruce-Firmware/src/modules/bjs_interpreter/storage_js.cpp directly, not
 * assumed). Furi's storage_file_open() has no equivalent -- it just fails if
 * the parent directory tree doesn't exist yet. Real scripts rely on the real
 * behavior without ever mkdir'ing themselves (the official App Store.js
 * writes straight to /BruceAppStore/cache/... and /BruceAppStore/
 * installed.json with no mkdir call anywhere in its own source -- confirmed
 * 2026-09-18 as the actual cause of "TypeError: cannot read property
 * 'length' of undefined" the moment a user selected any category: the cache
 * write silently failed (storage_file_open returned false, which this port's
 * native_storage_write already reported correctly, but the script's own code
 * never checks that return value), so the following read-back always threw
 * "file does not exist", the category variable was left at its initial `[]`,
 * and the very first render after that dereferenced `[].apps.length`). This
 * makes storage.write a genuine behavioral match, not just a same-named
 * function -- needed for ANY future script that writes to a nested path it
 * never explicitly creates, not just this one. */
static void bruce_js_mkdir_parents(Storage* storage, const char* path) {
    char buf[256];
    size_t len = strlen(path);
    if(len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, path, len);
    buf[len] = '\0';
    for(size_t i = 1; i < len; i++) {
        if(buf[i] == '/') {
            buf[i] = '\0';
            storage_common_mkdir(storage, buf);
            buf[i] = '/';
        }
    }
}

static const char* bruce_js_path_arg(JSContext* ctx, JSValue arg, JSCStringBuf* buf) {
    if(JS_IsString(ctx, arg)) {
        return JS_ToCString(ctx, arg, buf);
    }
    if(JS_IsObject(ctx, arg)) {
        JSValue v_path = JS_GetPropertyStr(ctx, arg, "path");
        if(JS_IsString(ctx, v_path)) {
            return JS_ToCString(ctx, v_path, buf);
        }
    }
    return NULL;
}

static JSValue native_storage_read(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    JSCStringBuf pb;
    const char* raw_path = argc >= 1 ? bruce_js_path_arg(ctx, argv[0], &pb) : NULL;
    if(!raw_path) return JS_ThrowTypeError(ctx, "storage.read: path required");

    FuriString* path = furi_string_alloc();
    bruce_js_resolve_path(raw_path, path);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FileInfo info;
    JSValue result;
    if(storage_common_stat(storage, furi_string_get_cstr(path), &info) != FSE_OK) {
        result = JS_ThrowTypeError(ctx, "storage.read: file does not exist: %s", raw_path);
    } else {
        File* file = storage_file_alloc(storage);
        if(!storage_file_open(file, furi_string_get_cstr(path), FSAM_READ, FSOM_OPEN_EXISTING)) {
            result = JS_ThrowTypeError(ctx, "storage.read: could not open %s", raw_path);
        } else {
            char* buf = malloc((size_t)info.size + 1);
            if(!buf) {
                result = JS_ThrowTypeError(ctx, "storage.read: out of memory");
            } else {
                size_t read = storage_file_read(file, buf, (size_t)info.size);
                result = JS_NewStringLen(ctx, buf, read);
                free(buf);
            }
        }
        storage_file_close(file);
        storage_file_free(file);
    }

    furi_record_close(RECORD_STORAGE);
    furi_string_free(path);
    return result;
}

/* Matches Bruce's own real default: storageWrite(path, data, mode) defaults
 * to APPEND, not overwrite, if mode is omitted (see storage_js.cpp's own
 * "const char *mode = FILE_APPEND; // default append") -- kept identical
 * here so a real script relying on that default behaves the same way. */
static JSValue native_storage_write(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    if(argc < 2 || !JS_IsString(ctx, argv[1])) {
        return JS_NewBool(false);
    }
    JSCStringBuf pb, db;
    const char* raw_path = bruce_js_path_arg(ctx, argv[0], &pb);
    size_t data_size = 0;
    const char* data = JS_ToCStringLen(ctx, &data_size, argv[1], &db);
    if(!raw_path || !data) return JS_NewBool(false);

    bool append = true;
    if(argc > 2 && JS_IsString(ctx, argv[2])) {
        JSCStringBuf mb;
        const char* mode = JS_ToCString(ctx, argv[2], &mb);
        if(mode && mode[0] == 'w') append = false;
    }

    FuriString* path = furi_string_alloc();
    bruce_js_resolve_path(raw_path, path);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    bruce_js_mkdir_parents(storage, furi_string_get_cstr(path));
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(
        file,
        furi_string_get_cstr(path),
        FSAM_WRITE,
        append ? FSOM_OPEN_APPEND : FSOM_CREATE_ALWAYS);
    if(ok) {
        storage_file_write(file, data, data_size);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    furi_string_free(path);
    return JS_NewBool(ok);
}

static JSValue native_storage_remove(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    JSCStringBuf pb;
    const char* raw_path = argc >= 1 ? bruce_js_path_arg(ctx, argv[0], &pb) : NULL;
    if(!raw_path) return JS_NewBool(false);

    FuriString* path = furi_string_alloc();
    bruce_js_resolve_path(raw_path, path);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool ok = storage_simply_remove(storage, furi_string_get_cstr(path));
    furi_record_close(RECORD_STORAGE);
    furi_string_free(path);
    return JS_NewBool(ok);
}

static JSValue native_storage_readdir(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    JSCStringBuf pb;
    const char* raw_path = argc >= 1 ? bruce_js_path_arg(ctx, argv[0], &pb) : NULL;
    if(!raw_path) return JS_NewArray(ctx, 0);

    bool with_file_types = false;
    if(argc > 1 && JS_IsObject(ctx, argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "withFileTypes");
        if(!JS_IsUndefined(v)) with_file_types = JS_ToBool(ctx, v) != 0;
    }

    FuriString* path = furi_string_alloc();
    bruce_js_resolve_path(raw_path, path);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    JSValue arr = JS_NewArray(ctx, 0);
    uint32_t index = 0;

    if(storage_dir_open(dir, furi_string_get_cstr(path))) {
        FileInfo info;
        char name[256];
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(with_file_types) {
                JSValue obj = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, name));
                JS_SetPropertyStr(ctx, obj, "size", JS_NewInt32(ctx, (int32_t)info.size));
                JS_SetPropertyStr(ctx, obj, "isDirectory", JS_NewBool(info.flags & FSF_DIRECTORY));
                JS_SetPropertyUint32(ctx, arr, index++, obj);
            } else {
                JS_SetPropertyUint32(ctx, arr, index++, JS_NewString(ctx, name));
            }
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
    furi_string_free(path);
    return arr;
}

/* wifi.connected() -- pure read of this port's own WiFi service state, never
 * touches the radio. Matches Bruce's real wifiConnected() (a cached bool, not
 * an attempt to connect) and, just as importantly, never risks silently
 * killing an active BLE session behind a script's back: this port enforces
 * "only one radio at a time" at the OS level (wifi_enable()'s own call chain,
 * wlan_hal_start(), unconditionally suspends BLE first -- see the
 * ble-wifi-mutual-exclusion memory) -- that suspend should only ever happen
 * because the USER explicitly turned WiFi on (long-press the Bluetooth tile
 * in Control Centre), never as a side effect of a script just checking
 * connectivity. A script that needs network access is expected to already be
 * online, exactly like a real Bruce user connects via the WiFi menu before
 * opening the App Store. */
static JSValue native_wifi_connected(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(ctx);
    UNUSED(this_val);
    UNUSED(argc);
    UNUSED(argv);
    Wifi* wifi = furi_record_open(RECORD_WIFI);
    bool connected = wifi_is_connected(wifi);
    furi_record_close(RECORD_WIFI);
    return JS_NewBool(connected);
}

/* wifi.httpFetch(url, options) -- the other real call the official Bruce App
 * Store script needs (see the bruce-js-engine-port memory's "real target,
 * found 2026-09-17" section, and the App-Store.js usage analysis: GET +
 * responseType "json" for the catalog, GET + save:{path,mode} for installing
 * a script file). Deliberately narrower than Bruce's real httpFetch (see
 * wifi_js.cpp) -- no request headers, no object/JSON request bodies, no
 * chunked-encoding growth beyond BRUCE_JS_HTTP_MAX_RESPONSE -- same
 * "implement only what's actually called" scoping already used for
 * storage.*. Never calls wifi_enable(): like connected() above, this assumes
 * the caller is already online -- Bruce's own real httpFetch calls
 * wifiConnectMenu() to prompt when offline, but this port has no synchronous
 * connect-picker dialog to call from inside a native binding, and silently
 * enabling the radio here would violate the WiFi/BLE mutual-exclusion rule
 * from behind the script's back. */
static JSValue native_wifi_http_fetch(JSContext* ctx, JSValue* this_val, int argc, JSValue* argv) {
    UNUSED(this_val);
    if(argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "httpFetch(url, options?)");
    }

    Wifi* wifi = furi_record_open(RECORD_WIFI);
    bool connected = wifi_is_connected(wifi);
    furi_record_close(RECORD_WIFI);
    if(!connected) {
        return JS_ThrowTypeError(ctx, "WiFi not connected");
    }

    JSCStringBuf ub;
    const char* url = JS_ToCString(ctx, argv[0], &ub);
    if(!url) return JS_ThrowTypeError(ctx, "httpFetch: invalid url");

    const char* method = "GET";
    int response_type = 0; /* 0=string, 1=json, 2=binary */
    const char* save_path = NULL;
    bool save_overwrite = true;
    const char* body = NULL;
    size_t body_len = 0;

    JSCStringBuf mb, rtb, bb, savb, pb, modeb;
    if(argc > 1 && JS_IsObject(ctx, argv[1])) {
        JSValue v_method = JS_GetPropertyStr(ctx, argv[1], "method");
        if(JS_IsString(ctx, v_method)) {
            const char* m = JS_ToCString(ctx, v_method, &mb);
            if(m) method = m;
        }
        JSValue v_rtype = JS_GetPropertyStr(ctx, argv[1], "responseType");
        if(JS_IsString(ctx, v_rtype)) {
            const char* r = JS_ToCString(ctx, v_rtype, &rtb);
            if(r && strcmp(r, "json") == 0) {
                response_type = 1;
            } else if(r && strcmp(r, "binary") == 0) {
                response_type = 2;
            }
        }
        JSValue v_body = JS_GetPropertyStr(ctx, argv[1], "body");
        if(JS_IsString(ctx, v_body)) {
            body = JS_ToCStringLen(ctx, &body_len, v_body, &bb);
        }
        JSValue v_save = JS_GetPropertyStr(ctx, argv[1], "save");
        if(JS_IsString(ctx, v_save)) {
            save_path = JS_ToCString(ctx, v_save, &savb);
        } else if(JS_IsObject(ctx, v_save)) {
            JSValue v_path = JS_GetPropertyStr(ctx, v_save, "path");
            if(JS_IsString(ctx, v_path)) {
                save_path = JS_ToCString(ctx, v_path, &pb);
            }
            JSValue v_mode = JS_GetPropertyStr(ctx, v_save, "mode");
            if(JS_IsString(ctx, v_mode)) {
                const char* mo = JS_ToCString(ctx, v_mode, &modeb);
                if(mo) save_overwrite = (mo[0] == 'w');
            }
        }
    }
    bool has_save = save_path != NULL;

    esp_http_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = url;
    cfg.timeout_ms = 30000;
    cfg.buffer_size = 2048;
    /* Matches this project's existing external-host convention
     * (wlan_sd_update.c's sd_update_http_cfg()) rather than requiring a CA
     * bundle: no crt_bundle_attach, TLS verification relaxed. Transport type
     * is left auto-detected (unlike sd_update, which always forces SSL) so a
     * script can fetch a plain http:// URL too -- the real App Store's own
     * data host is plain HTTP, not HTTPS. */
    cfg.skip_cert_common_name_check = true;
    cfg.crt_bundle_attach = NULL;
    cfg.use_global_ca_store = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(!client) return JS_ThrowInternalError(ctx, "httpFetch: client init failed");
    esp_http_client_set_method(
        client, strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);

    JSValue result = JS_UNDEFINED;
    if(esp_http_client_open(client, (int)body_len) != ESP_OK) {
        result = JS_ThrowInternalError(ctx, "httpFetch: connection failed");
    } else {
        if(body && body_len > 0) {
            esp_http_client_write(client, body, (int)body_len);
        }
        int64_t content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        size_t cap = content_length > 0 ? (size_t)content_length + 1 : 16 * 1024;
        if(cap > BRUCE_JS_HTTP_MAX_RESPONSE) cap = BRUCE_JS_HTTP_MAX_RESPONSE;
        char* payload = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        size_t total = 0;

        if(!payload) {
            result = JS_ThrowInternalError(ctx, "httpFetch: out of memory");
        } else {
            while(true) {
                if(total + 1 >= cap) {
                    if(cap >= BRUCE_JS_HTTP_MAX_RESPONSE) break;
                    size_t new_cap = cap * 2;
                    if(new_cap > BRUCE_JS_HTTP_MAX_RESPONSE) new_cap = BRUCE_JS_HTTP_MAX_RESPONSE;
                    char* grown = heap_caps_realloc(payload, new_cap, MALLOC_CAP_SPIRAM);
                    if(!grown) break;
                    payload = grown;
                    cap = new_cap;
                }
                int r = esp_http_client_read(client, payload + total, (int)(cap - 1 - total));
                if(r <= 0) break;
                total += (size_t)r;
            }
            payload[total] = '\0';

            if(has_save) {
                /* Reuses native_storage_write() directly (a plain C call, no
                 * JS-level dispatch needed) rather than duplicating its path
                 * resolution/file-open logic -- mirrors how Bruce's own real
                 * httpFetch delegates to native_storageWrite() for its save
                 * option. Defaults to overwrite (not storage.write()'s own
                 * append default) since "save this fresh download" is the
                 * only real caller and always means replace, not append. */
                JSValue storage_argv[3];
                storage_argv[0] = JS_NewString(ctx, save_path);
                storage_argv[1] = JS_NewStringLen(ctx, payload, total);
                storage_argv[2] = JS_NewString(ctx, save_overwrite ? "write" : "append");
                JSValue write_ok = native_storage_write(ctx, this_val, 3, storage_argv);

                JSValue obj = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, obj, "saved", write_ok);
                JS_SetPropertyStr(ctx, obj, "savedPath", JS_NewString(ctx, save_path));
                JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, status));
                JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(status >= 200 && status < 300));
                result = obj;
            } else {
                JSValue obj = JS_NewObject(ctx);
                if(response_type == 1) {
                    JSValue json_arg = JS_NewStringLen(ctx, payload, total);
                    JS_SetPropertyStr(ctx, obj, "body", js_json_parse(ctx, NULL, 1, &json_arg));
                } else if(response_type == 2) {
                    JS_SetPropertyStr(
                        ctx, obj, "body", JS_NewUint8ArrayCopy(ctx, (const uint8_t*)payload, total));
                } else {
                    JS_SetPropertyStr(ctx, obj, "body", JS_NewStringLen(ctx, payload, total));
                }
                JS_SetPropertyStr(ctx, obj, "length", JS_NewInt32(ctx, (int32_t)total));
                JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, status));
                JS_SetPropertyStr(ctx, obj, "response", JS_NewInt32(ctx, status));
                JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(status >= 200 && status < 300));
                result = obj;
            }
            heap_caps_free(payload);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

#include "bruce_stdlib.h"

/* Returns the printable result/error line so both the CLI pipe (visible even
 * without a log monitor attached) and the log (FURI_LOG_I below) show it.
 *
 * Earlier revisions kept every hardware-touching binding (subghz, gpio) out
 * of bruce_js_app()'s own menu-launch/resume-app path, suspecting the early
 * boot/resume timing itself was unsafe. That theory was wrong -- the actual
 * crash was native_subghz_is_frequency_valid() missing subghz_devices_init()/
 * deinit() (see that function's comment), unrelated to which context called
 * it. With that fixed, any script -- including ones loaded from a real .bjs
 * file via bruce_js_run_file() below -- is safe to run from this app's
 * normal launch path like any other Flipper app. */
static bool bruce_js_run_test(char* out, size_t out_size, const char* script) {
    void* arena = heap_caps_malloc(BRUCE_JS_ARENA_SIZE, MALLOC_CAP_SPIRAM);
    if(!arena) {
        snprintf(out, out_size, "Failed to allocate %d byte arena", BRUCE_JS_ARENA_SIZE);
        return false;
    }

    JSContext* ctx = JS_NewContext(arena, BRUCE_JS_ARENA_SIZE, &bruce_stdlib);
    if(!ctx) {
        snprintf(out, out_size, "JS_NewContext failed");
        heap_caps_free(arena);
        return false;
    }

    /* BRUCE_PRICOLOR/SECCOLOR/BGCOLOR -- real Bruce sets these three as bare
     * globals at interpreter startup (Bruce-Firmware's own interpreter.cpp:
     * JS_SetPropertyStr(ctx, global, "BRUCE_PRICOLOR", ...)), from the
     * user's selected theme, not a static stdlib entry -- found missing
     * 2026-09-17 running the real App-Store.js, which references
     * BRUCE_PRICOLOR directly for its own title text color. This port has
     * no per-user theme selection to read, so these are the same fixed
     * values Bruce-Firmware/src/core/theme.h ships as its own defaults
     * (DEFAULT_PRICOLOR 0xA80F, secColor = priColor-0x2000, bgColor 0x0000)
     * -- raw RGB565 bit patterns, byte-swapped the same way every other
     * color value in this file is (bruce_js_pack_swap), since that's the
     * byte order this port's own framebuffer/panel actually wants. */
    JSValue ctx_global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, ctx_global, "BRUCE_PRICOLOR", JS_NewInt32(ctx, (int32_t)bruce_js_swap16(0xA80F)));
    JS_SetPropertyStr(ctx, ctx_global, "BRUCE_SECCOLOR", JS_NewInt32(ctx, (int32_t)bruce_js_swap16(0x880F)));
    JS_SetPropertyStr(ctx, ctx_global, "BRUCE_BGCOLOR", JS_NewInt32(ctx, (int32_t)bruce_js_swap16(0x0000)));

    /* Set up this run's display/keyboard state -- only meaningful when the
     * caller already has a direct-draw session active (bruce_js_app()'s own
     * menu/browser/result loop sets g_script_ctx.gui_active before calling
     * in here); the "brucejs" CLI path leaves it false, so the display and
     * keyboard bindings silently no-op instead of touching GUI/input
     * hardware from a context that isn't holding it. */
    if(g_script_ctx.gui_active) {
        g_script_ctx.fb_w = furi_hal_display_get_h_res();
        g_script_ctx.fb_h = furi_hal_display_get_v_res();
        g_script_ctx.fb = heap_caps_malloc(
            (size_t)g_script_ctx.fb_w * g_script_ctx.fb_h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        g_script_ctx.text_color = bruce_js_pack_swap(0xff, 0xff, 0xff);
        g_script_ctx.text_scale = 1;
        g_script_ctx.text_align_h = 0;
        g_script_ctx.text_align_v = 0;
        g_script_ctx.esc = false;
        g_script_ctx.next = false;
        g_script_ctx.prev = false;
        g_script_ctx.sel = false;
        g_script_ctx.any = false;
        if(g_script_ctx.fb) {
            bruce_js_fb_fill(g_script_ctx.fb, g_script_ctx.fb_w, g_script_ctx.fb_h, 0);
        }
        g_script_ctx.input_queue = furi_message_queue_alloc(16, sizeof(InputEvent));
        g_script_ctx.input_sub = furi_pubsub_subscribe(
            g_script_ctx.input_events, bruce_js_script_input_callback, g_script_ctx.input_queue);
    }

    bool ok = false;
    JSValue result = JS_Eval(ctx, script, strlen(script), "bruce_js_test", JS_EVAL_RETVAL);

    if(JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        if(JS_IsError(ctx, exc)) {
            /* Phase 3.5 originally documented .name/.message as unreachable
             * here and fell back to a generic "something threw" message --
             * that was a misdiagnosis. The real cause was the same bug that
             * segfaulted ANY primitive property access (see gen/
             * bruce_stdlib_gen.c's big comment on the core-language stdlib
             * table): Error's own class/prototype was never registered, so
             * JS_GetPropertyStr(exc, "message") had no js_error_get_message
             * cgetset to resolve to. Now that Error (+ its 7 subclasses) is
             * registered, this reads real messages -- confirmed via the host
             * test harness on a plain `throw new Error("x")`. */
            JSValue name = JS_GetPropertyStr(ctx, exc, "name");
            JSValue message = JS_GetPropertyStr(ctx, exc, "message");
            JSCStringBuf nb, mb;
            const char* n = JS_ToCString(ctx, name, &nb);
            const char* m = JS_ToCString(ctx, message, &mb);
            snprintf(
                out,
                out_size,
                "Script threw: %s: %s",
                n ? n : "Error",
                m ? m : "(no message)");
        } else {
            JSCStringBuf buf;
            const char* msg = JS_ToCString(ctx, exc, &buf);
            snprintf(out, out_size, "Script threw: %s", msg ? msg : "(non-Error exception)");
        }
    } else {
        ok = true;
        /* JS_ToInt32 performs ECMAScript ToInt32 coercion, which NEVER fails
         * (a non-numeric string coerces to 0, it doesn't return an error) --
         * checking its return code to guess "was this numeric?" is wrong and
         * silently misrenders any string result as "0". Check the actual
         * value type instead. */
        if(JS_IsNumber(ctx, result)) {
            int ival = 0;
            JS_ToInt32(ctx, &ival, result);
            snprintf(out, out_size, "Script result: %ld", (long)ival);
        } else {
            JSCStringBuf buf;
            const char* s = JS_ToCString(ctx, result, &buf);
            snprintf(out, out_size, "Script result: %s", s ? s : "(unknown)");
        }
    }

    if(g_script_ctx.gui_active) {
        if(g_script_ctx.input_sub) {
            furi_pubsub_unsubscribe(g_script_ctx.input_events, g_script_ctx.input_sub);
            g_script_ctx.input_sub = NULL;
        }
        if(g_script_ctx.input_queue) {
            furi_message_queue_free(g_script_ctx.input_queue);
            g_script_ctx.input_queue = NULL;
        }
        if(g_script_ctx.fb) {
            heap_caps_free(g_script_ctx.fb);
            g_script_ctx.fb = NULL;
        }
    }

    JS_FreeContext(ctx);
    heap_caps_free(arena);
    return ok;
}

/* Reads a .bjs/.js file from storage and runs it the same way bruce_js_run_test()
 * runs a hardcoded string -- this is the "App Launch" path: real Bruce scripts,
 * not just inline sanity checks. */
static bool bruce_js_run_file(char* out, size_t out_size, const char* path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool ok = false;

    FileInfo file_info;
    if(storage_common_stat(storage, path, &file_info) != FSE_OK) {
        snprintf(out, out_size, "Cannot stat %s", path);
    } else if(file_info.size == 0 || file_info.size > BRUCE_JS_MAX_SCRIPT_SIZE) {
        snprintf(out, out_size, "Script empty or too large (max %d bytes)", BRUCE_JS_MAX_SCRIPT_SIZE);
    } else if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        snprintf(out, out_size, "Cannot open %s", path);
    } else {
        char* script = heap_caps_malloc((size_t)file_info.size + 1, MALLOC_CAP_SPIRAM);
        if(!script) {
            snprintf(out, out_size, "Out of memory reading script");
        } else {
            size_t read = storage_file_read(file, script, (size_t)file_info.size);
            script[read] = '\0';
            ok = bruce_js_run_test(out, out_size, script);
            heap_caps_free(script);
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

/* App Store bootstrap: streams a plain HTTP(S) GET straight to a file via
 * Furi's storage_file_* primitives, with NO JSContext/JSValue involved --
 * this runs from bruce_js_app()'s menu loop, before any script (and its
 * JSContext) exists. native_wifi_http_fetch() can't be reused here: it needs
 * a JSContext for its `save` option's native_storage_write() call, and it
 * buffers the WHOLE response in one PSRAM allocation first, which is fine
 * for its own 512KB cap but wasteful for a plain download-then-run
 * bootstrap. wlan_sd_update.c already solves this same "stream a GET to a
 * file" problem (sd_update_download_file()), but that function is `static`
 * (not exported), takes a WlanSdUpdate* progress/cancel handle for its own
 * background xTaskCreate task, and does HTTP-Range resume + multi-retry --
 * machinery sized for a ~13MB multi-file mirror sync, not a single ~40KB
 * one-shot fetch triggered synchronously from a menu selection. This mirrors
 * that file's TLS convention (sd_update_http_cfg(): skip_cert_common_name_
 * check, no crt_bundle_attach, no global CA store) without the resume/retry/
 * progress apparatus.
 *
 * Bounded at BRUCE_JS_MAX_SCRIPT_SIZE: no point writing more than
 * bruce_js_run_file() will ever accept. On ANY failure (bad HTTP status,
 * read error, short/dropped transfer, over-size, file-open/write failure)
 * the destination is removed rather than left partially written, so a later
 * retry sees "file absent" and re-downloads instead of mistaking a
 * truncated file for an already-installed one. */
static bool bruce_js_download_to_file(
    char* out, size_t out_size, const char* url, const char* dest) {
    esp_http_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = url;
    cfg.timeout_ms = 30000;
    cfg.buffer_size = 2048;
    cfg.skip_cert_common_name_check = true;
    cfg.crt_bundle_attach = NULL;
    cfg.use_global_ca_store = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(!client) {
        snprintf(out, out_size, "App Store: HTTP client init failed");
        return false;
    }

    bool ok = false;
    bool complete = false;
    if(esp_http_client_open(client, 0) != ESP_OK) {
        snprintf(out, out_size, "App Store: connection failed");
    } else {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if(status != 200) {
            snprintf(out, out_size, "App Store: HTTP %d", status);
        } else {
            Storage* storage = furi_record_open(RECORD_STORAGE);
            File* file = storage_file_alloc(storage);
            if(!storage_file_open(file, dest, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
                snprintf(out, out_size, "App Store: cannot create %s", dest);
            } else {
                uint8_t* chunk = malloc(2048);
                if(!chunk) {
                    snprintf(out, out_size, "App Store: out of memory");
                } else {
                    ok = true;
                    size_t total = 0;
                    while(true) {
                        int r = esp_http_client_read(client, (char*)chunk, 2048);
                        if(r < 0) {
                            snprintf(out, out_size, "App Store: read error");
                            ok = false;
                            break;
                        }
                        if(r == 0) {
                            complete = esp_http_client_is_complete_data_received(client);
                            if(!complete) snprintf(out, out_size, "App Store: connection dropped");
                            break;
                        }
                        total += (size_t)r;
                        if(total > BRUCE_JS_MAX_SCRIPT_SIZE) {
                            snprintf(out, out_size, "App Store: download exceeded %d bytes",
                                     BRUCE_JS_MAX_SCRIPT_SIZE);
                            ok = false;
                            break;
                        }
                        if(storage_file_write(file, chunk, (size_t)r) != (size_t)r) {
                            snprintf(out, out_size, "App Store: write error (SD full?)");
                            ok = false;
                            break;
                        }
                    }
                    free(chunk);
                }
                storage_file_close(file);
            }
            storage_file_free(file);
            furi_record_close(RECORD_STORAGE);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ok = ok && complete;
    if(!ok) {
        /* Never leave a partial/truncated file behind -- a later launch's
         * "does it already exist" check must see "absent", not a stale
         * corrupt file it would mistake for an already-good install. */
        Storage* storage = furi_record_open(RECORD_STORAGE);
        storage_common_remove(storage, dest);
        furi_record_close(RECORD_STORAGE);
    }
    return ok;
}

/* Status bar -- matches real Bruce's own drawStatusBar() geometry EXACTLY
 * (Bruce-Firmware/src/core/display.cpp:921-1004), pixel-for-pixel, not just
 * structurally: this port's screen is 320x170, the SAME resolution Bruce
 * itself assumes on this exact board (boards/lilygo-t-embed-cc1101/
 * pins_arduino.h: TFT_WIDTH 170/TFT_HEIGHT 320, swapped for landscape), so
 * Bruce's own absolute pixel constants apply unmodified instead of needing
 * to be rederived:
 *   drawRoundRect(5, 5, w-10, h-10, 5, priColor)   -- outer frame
 *   drawLine(5, 25, w-6, 25, priColor)              -- separator under it
 *   corner label at (12, 12), text size 1           -- "BRUCE <ver>" for real
 *                                                       Bruce; "BRUCE JS" here
 * The bold, auto-sized, centered per-screen TITLE ("MAIN MENU", a filename,
 * etc.) is a SEPARATE call, bruce_js_draw_title() below -- matching how real
 * Bruce splits drawStatusBar() (this function) from printTitle() (a
 * different function, called by whichever screen needs a title). Earlier
 * revisions conflated the two into one fixed "BRUCE JS" title glued to this
 * chrome; splitting them is what real Bruce's own screens do (rewritten
 * 2026-09-17 to close that fidelity gap -- see the bruce-js-engine-port
 * memory's "faithfully recreate" recommendation). */
static void bruce_js_draw_status_bar(uint16_t* fb, int w, int h) {
    const uint16_t bg = bruce_js_pack_swap(0x10, 0x10, 0x18);
    const uint16_t accent = bruce_js_accent_color();

    bruce_js_fb_draw_round_rect(fb, w, h, 5, 5, w - 10, h - 10, 5, accent, bg);
    bruce_js_fb_fill_rect(fb, w, h, 5, 25, w - 10, 1, accent);

    bruce_js_fb_draw_text(fb, w, h, 12, 12, "BRUCE JS", accent, 1);
}

/* printTitle() equivalent (display.cpp:1028-1044): uppercases the string,
 * starts at size 2 and shrinks to size 1 if it wouldn't fit within a 10px
 * margin on each side, centers horizontally, fixed y=28 (Bruce's
 * BORDER_PAD_Y). Bruce's own font cell (LW=6,LH=8) differs from this port's
 * 5x7 bitmap font, so exact character counts-per-width differ slightly, but
 * the ALGORITHM and Y position are copied verbatim. */
static void bruce_js_draw_title(uint16_t* fb, int w, int h, const char* title) {
    const uint16_t accent = bruce_js_accent_color();
    char upper[64];
    size_t len = strlen(title);
    if(len >= sizeof(upper)) len = sizeof(upper) - 1;
    for(size_t i = 0; i < len; i++) {
        char c = title[i];
        upper[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    upper[len] = '\0';

    int title_scale = 2;
    while(title_scale > 1 &&
          (int)len * title_scale * (BRUCE_FONT_CHAR_W + 1) > w - 2 * 10) {
        title_scale--;
    }
    int title_w = (int)len * title_scale * (BRUCE_FONT_CHAR_W + 1);
    bruce_js_fb_draw_text(fb, w, h, (w - title_w) / 2, 28, upper, accent, title_scale);
}

static void bruce_js_draw_screen(const char* result_line) {
    uint16_t w = furi_hal_display_get_h_res();
    uint16_t h = furi_hal_display_get_v_res();
    uint16_t* fb = heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if(!fb) {
        FURI_LOG_E(TAG, "Out of PSRAM for %ux%u screen buffer", w, h);
        return;
    }

    const uint16_t bg = bruce_js_pack_swap(0x10, 0x10, 0x18);
    const uint16_t body_fg = bruce_js_pack_swap(0xff, 0xff, 0xff);
    const uint16_t hint_fg = bruce_js_pack_swap(0x90, 0x90, 0xa0);

    bruce_js_fb_fill(fb, w, h, bg);
    bruce_js_draw_status_bar(fb, w, h);
    bruce_js_draw_title(fb, w, h, "Result");

    bruce_js_fb_draw_wrapped(fb, w, h, 10, 50, (int)w - 20, 16, result_line, body_fg, 2);

    const char* hint = "Press BACK to exit";
    int hint_w = (int)strlen(hint) * (BRUCE_FONT_CHAR_W + 1);
    bruce_js_fb_draw_text(fb, w, h, ((int)w - hint_w) / 2, (int)h - 12, hint, hint_fg, 1);

    furi_hal_display_blit_rgb565(0, 0, w, h, fb);
    free(fb);
}

/* Toast/status popup -- matches real Bruce's displayRedStripe() geometry
 * (display.cpp:165-208, the function behind displayError/Warning/Info/
 * Success/TextLine): a rounded box inset 10px each side, vertically
 * centered, radius 7, height 26 for one line (this port doesn't replicate
 * the multi-line auto-height math since every caller here is a short status
 * line, not a paragraph). Used for WiFi connect status ("Scanning...",
 * "Connecting...", failure/success) so those screens look like the SAME
 * "Bruce popup" family instead of a one-off dialog shape. */
static void bruce_js_draw_toast(const char* text, uint16_t fgcolor, uint16_t bgcolor) {
    uint16_t w = furi_hal_display_get_h_res();
    uint16_t h = furi_hal_display_get_v_res();
    uint16_t* fb = heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if(!fb) return;

    const uint16_t bg = bruce_js_pack_swap(0x10, 0x10, 0x18);
    bruce_js_fb_fill(fb, w, h, bg);
    bruce_js_draw_status_bar(fb, w, h);

    int box_h = 26;
    int box_y = (int)h / 2 - box_h / 2;
    bruce_js_fb_fill_round_rect(fb, w, h, 10, box_y, (int)w - 20, box_h, 7, bgcolor);
    int text_w = (int)strlen(text) * (BRUCE_FONT_CHAR_W + 1);
    bruce_js_fb_draw_text(fb, w, h, ((int)w - text_w) / 2, box_y + (box_h - 7) / 2, text, fgcolor, 1);

    furi_hal_display_blit_rgb565(0, 0, w, h, fb);
    free(fb);
}

#define BRUCE_JS_LIST_LABEL_LEN  40
#define BRUCE_JS_LIST_VISIBLE_ROWS 5

/* Stripped Bruce-style scrollable list -- matches real Bruce's own
 * drawOptions() geometry (display.cpp:764-857) directly, since this port's
 * row height (20px) happens to equal Bruce's own `FM*8+4` exactly (FM=2 on
 * this board): box is 10%-of-width margin each side (Bruce: `tftWidth*0.10`),
 * VERTICALLY CENTERED via `h/2 - visibleRows*20/2 - 5` (was a fixed y=50
 * before this pass -- Bruce always centers the box regardless of item
 * count), height `20*visibleRows + 10`, radius 5. The selected row's
 * highlight bar is itself a ROUNDED rect (Bruce: radius 3, "erase old row /
 * draw new row" diffing -- this port just redraws the whole box every frame,
 * simpler and fast enough at this row count) with inverted text -- matching
 * Bruce's `setTextColor(bgcolor, priColor)` (text drawn in the background
 * color, cut out of the highlight fill) rather than pure black. */
static void bruce_js_draw_list(
    uint16_t* fb,
    int w,
    int h,
    const char* title,
    const char labels[][BRUCE_JS_LIST_LABEL_LEN],
    int count,
    int selected) {
    const uint16_t bg = bruce_js_pack_swap(0x10, 0x10, 0x18);
    const uint16_t accent = bruce_js_accent_color();
    const uint16_t item_fg = bruce_js_pack_swap(0xff, 0xff, 0xff);
    const uint16_t hint_fg = bruce_js_pack_swap(0x90, 0x90, 0xa0);

    bruce_js_fb_fill(fb, w, h, bg);
    bruce_js_draw_status_bar(fb, w, h);
    bruce_js_draw_title(fb, w, h, title);

    const int row_h = 20;
    int visible_rows = count < BRUCE_JS_LIST_VISIBLE_ROWS ? count : BRUCE_JS_LIST_VISIBLE_ROWS;
    if(visible_rows < 1) visible_rows = 1;
    int box_x = w / 10;
    int box_w = w - 2 * box_x;
    int box_h = row_h * visible_rows + 10;
    int box_y = h / 2 - visible_rows * row_h / 2 - 5;
    if(box_y < 44) box_y = 44; /* never overlap the title (y=28 + its line height) */
    bruce_js_fb_draw_round_rect(fb, w, h, box_x, box_y, box_w, box_h, 5, accent, bg);

    int top = selected - visible_rows / 2;
    if(top > count - visible_rows) top = count - visible_rows;
    if(top < 0) top = 0;

    for(int row = 0; row < visible_rows; row++) {
        int i = top + row;
        if(i >= count) break;
        int row_y = box_y + 5 + row * row_h;
        char label[BRUCE_JS_LIST_LABEL_LEN + 2];
        if(i == selected) {
            bruce_js_fb_fill_round_rect(
                fb, w, h, box_x + 2, row_y, box_w - 4, row_h - 2, 3, accent);
            snprintf(label, sizeof(label), "> %s", labels[i]);
        } else {
            snprintf(label, sizeof(label), "  %s", labels[i]);
        }
        bruce_js_fb_draw_text(
            fb, w, h, box_x + 10, row_y + 5, label, i == selected ? bg : item_fg, 1);
    }

    const char* hint = count > 0 ? "UP/DOWN select, OK choose, BACK exit" : "Empty -- BACK to exit";
    int hint_w = (int)strlen(hint) * (BRUCE_FONT_CHAR_W + 1);
    bruce_js_fb_draw_text(fb, w, h, ((int)w - hint_w) / 2, (int)h - 12, hint, hint_fg, 1);
}

typedef struct {
    FuriSemaphore* exit_sem;
} BruceJsScreen;

static void bruce_js_screen_input_callback(const void* value, void* context) {
    BruceJsScreen* screen = context;
    const InputEvent* event = value;
    if(event->key == InputKeyBack && event->type == InputTypeShort) {
        furi_semaphore_release(screen->exit_sem);
    }
}

static void bruce_js_list_input_callback(const void* value, void* context) {
    FuriMessageQueue* queue = context;
    furi_message_queue_put(queue, value, FuriWaitForever);
}

/* Runs a list's own input loop (Up/Down move selection, OK confirms, Back
 * cancels) using a message queue fed by a temporary input subscription --
 * same raw RECORD_INPUT_EVENTS mechanism the result screen's Back-to-exit
 * wait already uses, just extended to more keys and re-drawn per move.
 * Returns the chosen index, or -1 if the user pressed Back. Caller must
 * already hold gui_direct_draw_acquire(). */
static int bruce_js_list_run(
    FuriPubSub* input_events,
    const char* title,
    const char labels[][BRUCE_JS_LIST_LABEL_LEN],
    int count) {
    uint16_t w = furi_hal_display_get_h_res();
    uint16_t h = furi_hal_display_get_v_res();
    uint16_t* fb = heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if(!fb) {
        FURI_LOG_E(TAG, "Out of PSRAM for %ux%u list buffer", w, h);
        return -1;
    }

    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    FuriPubSubSubscription* sub =
        furi_pubsub_subscribe(input_events, bruce_js_list_input_callback, queue);

    int selected = 0;
    int result = -1;
    bruce_js_draw_list(fb, w, h, title, labels, count, selected);
    furi_hal_display_blit_rgb565(0, 0, w, h, fb);

    InputEvent event;
    while(furi_message_queue_get(queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event.type != InputTypeShort && event.type != InputTypeRepeat) continue;

        if(event.key == InputKeyBack) {
            break;
        } else if(count > 0 && event.key == InputKeyUp) {
            selected = (selected - 1 + count) % count;
        } else if(count > 0 && event.key == InputKeyDown) {
            selected = (selected + 1) % count;
        } else if(count > 0 && event.key == InputKeyOk) {
            result = selected;
            break;
        } else {
            continue;
        }
        bruce_js_draw_list(fb, w, h, title, labels, count, selected);
        furi_hal_display_blit_rgb565(0, 0, w, h, fb);
    }

    furi_pubsub_unsubscribe(input_events, sub);
    furi_message_queue_free(queue);
    free(fb);
    return result;
}

#define BRUCE_JS_MAX_DIR_ENTRIES 48

typedef struct {
    char name[256]; /* matches storage_dir_read()'s own name buffer size below */
    bool is_dir;
} BruceJsDirEntry;

/* Lists one directory's `.js`/`.bjs` files and sub-folders (folders always
 * kept, for navigating into category folders like the real App Store's own
 * "/BruceJS/[category]/" layout) -- plain native C, not the JS-facing
 * storage.readdir() binding (that one is scoped to what the App Store
 * SCRIPT calls; this is what Bruce JS's own UI calls to draw itself). */
static int bruce_js_list_dir(const char* path, BruceJsDirEntry* entries, int max_entries) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    int count = 0;

    if(storage_dir_open(dir, path)) {
        FileInfo info;
        char name[256];
        while(count < max_entries && storage_dir_read(dir, &info, name, sizeof(name))) {
            bool is_dir = (info.flags & FSF_DIRECTORY) != 0;
            bool keep = is_dir;
            if(!keep) {
                size_t len = strlen(name);
                keep = (len > 3 && strcmp(name + len - 3, ".js") == 0) ||
                       (len > 4 && strcmp(name + len - 4, ".bjs") == 0);
            }
            if(keep) {
                snprintf(entries[count].name, sizeof(entries[count].name), "%s", name);
                entries[count].is_dir = is_dir;
                count++;
            }
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
    return count;
}

/* Add-from-File flow: Bruce JS's OWN raw-RGB file/folder browser, not
 * Flipper's system dialog_file_browser_show(). Two real bugs came from
 * reusing that system dialog: (1) it shares the same GuiLayerFullscreen
 * layer this app's raw-RGB screens implicitly occupy, so releasing direct
 * draw right before showing it left a gap where gui_redraw() fell through to
 * the Desktop/lock scene for a frame (the "loader unlocked" flash); a
 * placeholder-ViewPort fix for that then regressed into the dialog never
 * getting focus at all (stuck blank). (2) it's Flipper's own visual style
 * regardless, which is the opposite of what this app is for. A small native
 * browser avoids both: it never leaves direct-draw mode at all, and it's
 * drawn with the exact same bruce_js_draw_list() used everywhere else in
 * this app. Folders descend with OK; Back goes up one level, or exits the
 * browser entirely from BRUCE_JS_SCRIPTS_FOLDER itself. Returns true if a
 * script was picked and run (regardless of whether the SCRIPT itself
 * succeeded -- `*ok` carries that). Caller must already hold
 * gui_direct_draw_acquire(). */
static bool bruce_js_browse_files(FuriPubSub* input_events, char* line, size_t line_size, bool* ok) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, BRUCE_JS_SCRIPTS_FOLDER);
    furi_record_close(RECORD_STORAGE);

    FuriString* current_dir = furi_string_alloc_set(BRUCE_JS_SCRIPTS_FOLDER);
    bool picked = false;

    while(true) {
        BruceJsDirEntry entries[BRUCE_JS_MAX_DIR_ENTRIES];
        int count = bruce_js_list_dir(furi_string_get_cstr(current_dir), entries, BRUCE_JS_MAX_DIR_ENTRIES);

        char labels[BRUCE_JS_MAX_DIR_ENTRIES][BRUCE_JS_LIST_LABEL_LEN];
        for(int i = 0; i < count; i++) {
            snprintf(
                labels[i],
                sizeof(labels[i]),
                entries[i].is_dir ? "%s/" : "%s",
                entries[i].name);
        }

        int selection = bruce_js_list_run(input_events, "Add from File", labels, count);

        if(selection < 0) {
            if(furi_string_equal(current_dir, BRUCE_JS_SCRIPTS_FOLDER)) {
                break;
            }
            FuriString* parent = furi_string_alloc();
            path_extract_dirname(furi_string_get_cstr(current_dir), parent);
            furi_string_move(current_dir, parent);
            continue;
        }

        furi_string_cat_printf(current_dir, "/%s", entries[selection].name);
        if(entries[selection].is_dir) {
            continue;
        }

        *ok = bruce_js_run_file(line, line_size, furi_string_get_cstr(current_dir));
        picked = true;
        break;
    }

    furi_string_free(current_dir);
    return picked;
}

#define BRUCE_JS_WIFI_MAX_NETWORKS      32
#define BRUCE_JS_WIFI_CONNECT_TIMEOUT_MS 10000

/* Bruce-style WiFi connect popup -- real Bruce's own wifiConnectMenu()
 * (Bruce-Firmware/src/core/wifi/wifi_common.cpp) never sends the user
 * somewhere else to get online: it scans and shows a network list in
 * Bruce's own UI, right there, then connects. This port's previous behavior
 * for the same situation (a script needing WiFi while offline) was to just
 * throw "WiFi not connected" from native_wifi_http_fetch() and leave the
 * user to find Flipper's own separate WiFi settings screen -- flagged
 * explicitly as not matching Bruce's UX. This is the equivalent: scans via
 * wlan_hal_scan(), shows a Bruce-styled list (reusing bruce_js_list_run(),
 * same as every other list in this app), and for a network with an
 * already-saved password (wlan_password_read() -- the same per-SSID
 * convention applications/main/streaming/scenes/scene_wifi_connect.c uses),
 * connects and polls for success, calling wifi_mark_connected() on success
 * (the exact call that scene makes, so the connection stays sticky/global
 * after this app exits) -- mirroring that scene's whole connect-and-poll
 * pattern rather than inventing a new one.
 *
 * Scope limit, matching this session's "implement only what's actually
 * needed" precedent used throughout: a network with NO saved password shows
 * a message asking the user to connect it via Flipper's WiFi settings once
 * first, rather than a crash or a silent no-op. Real Bruce's own
 * wifiConnectMenu() has an on-screen password keyboard via its own
 * keyboard() helper; building an equivalent raw-RGB text-entry widget for
 * this board's 4-button (Up/Down/OK/Back) input is real new UI work, not a
 * quick reuse of an existing primitive like everything else here -- worth
 * doing as a dedicated follow-up if this scope limit is actually hit often.
 *
 * Deliberately called only from the app's own menu (see the "App Store"
 * branch in bruce_js_app() below), never from inside native_wifi_http_fetch()
 * itself: that binding can be called from anywhere in a running script, and
 * driving a full blocking UI flow with its own input loop from inside a
 * native call would fight with whatever the script's own display/keyboard
 * usage is doing. Triggering this at App-Store-LAUNCH time instead (before
 * any script runs) is both simpler and matches the user's own framing --
 * "launch App Store will use Bruce popup", not "any script calling
 * httpFetch mid-run pops this up". Returns true once wifi_is_connected() is
 * actually true; false otherwise (Back on the list, no networks, no saved
 * password, or a connect timeout). Caller must already hold
 * gui_direct_draw_acquire(), same as every other screen in this file. */
static bool bruce_js_wifi_ensure_connected(FuriPubSub* input_events) {
    Wifi* wifi = furi_record_open(RECORD_WIFI);
    bool already = wifi_is_connected(wifi);
    furi_record_close(RECORD_WIFI);
    if(already) return true;

    const uint16_t toast_white = bruce_js_pack_swap(0xff, 0xff, 0xff);
    const uint16_t toast_red = bruce_js_pack_swap(0x80, 0x00, 0x00);
    const uint16_t toast_green = bruce_js_pack_swap(0x00, 0x60, 0x00);
    const uint16_t accent = bruce_js_accent_color();

    bruce_js_draw_toast("Scanning...", toast_white, accent);

    /* wlan_hal_scan() returns false immediately if the radio isn't already
     * running (checks its own internal s_started flag) -- found missing
     * 2026-09-17 when a real hardware test reported "no networks found"
     * every time: this board boots with WiFi off ("WlanHal: WiFi remains
     * off until enabled from Control"), so a script/app launched fresh
     * always hit this. scene_wifi_connect.c's own real connect flow starts
     * the radio right before connecting; scanning needs the exact same
     * start-if-not-already-started call, just earlier. */
    if(!wlan_hal_is_started()) wlan_hal_start();

    wifi_ap_record_t* records = NULL;
    uint16_t count = 0;
    bool scanned = wlan_hal_scan(&records, &count, BRUCE_JS_WIFI_MAX_NETWORKS);
    if(!scanned || count == 0) {
        if(records) free(records);
        bruce_js_draw_toast("No networks found", toast_white, toast_red);
        furi_delay_ms(1500);
        return false;
    }

    int usable = count < BRUCE_JS_WIFI_MAX_NETWORKS ? count : BRUCE_JS_WIFI_MAX_NETWORKS;
    char labels[BRUCE_JS_WIFI_MAX_NETWORKS][BRUCE_JS_LIST_LABEL_LEN];
    for(int i = 0; i < usable; i++) {
        bool open = records[i].authmode == WIFI_AUTH_OPEN;
        /* SSID capped at %.24s -- provably fits BRUCE_JS_LIST_LABEL_LEN (40)
         * together with the "#"/rssi/parens even for a full 32-byte SSID,
         * where the uncapped %s tripped -Werror=format-truncation. */
        snprintf(
            labels[i],
            sizeof(labels[i]),
            "%s%.24s (%d)",
            open ? "" : "#",
            (const char*)records[i].ssid,
            (int)records[i].rssi);
    }

    int selection = bruce_js_list_run(input_events, "WiFi Networks", labels, usable);
    if(selection < 0) {
        free(records);
        return false;
    }

    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s", (const char*)records[selection].ssid);
    bool open = records[selection].authmode == WIFI_AUTH_OPEN;
    uint8_t channel = records[selection].primary;
    uint8_t bssid[6];
    memcpy(bssid, records[selection].bssid, sizeof(bssid));
    free(records);

    char password[64] = {0};
    bool have_password = open || wlan_password_read(ssid, password, sizeof(password));
    if(!have_password) {
        char msg[96];
        snprintf(msg, sizeof(msg), "No saved password for %s", ssid);
        bruce_js_draw_toast(msg, toast_white, toast_red);
        furi_delay_ms(2000);
        return false;
    }

    char connecting_msg[64];
    snprintf(connecting_msg, sizeof(connecting_msg), "Connecting to %s...", ssid);
    bruce_js_draw_toast(connecting_msg, toast_white, accent);

    if(!wlan_hal_is_started()) wlan_hal_start();
    wlan_hal_connect(ssid, password, bssid, channel);

    bool connected = false;
    for(int waited = 0; waited < BRUCE_JS_WIFI_CONNECT_TIMEOUT_MS; waited += 250) {
        if(wlan_hal_is_connected()) {
            connected = true;
            break;
        }
        furi_delay_ms(250);
    }

    if(connected) {
        Wifi* wifi2 = furi_record_open(RECORD_WIFI);
        wifi_mark_connected(wifi2, ssid);
        furi_record_close(RECORD_WIFI);
        bruce_js_draw_toast("Connected!", toast_white, toast_green);
        furi_delay_ms(1000);
    } else {
        wlan_hal_disconnect();
        bruce_js_draw_toast("Connection failed", toast_white, toast_red);
        furi_delay_ms(1500);
    }

    return connected;
}

/* Shared by the "App Store" and "Update App Store" menu entries -- connects
 * WiFi, ensures the bootstrap script is present, and runs it. With
 * `force_update` true, always re-downloads (overwriting any cached copy)
 * instead of reusing it -- the only way to pick up a newer version the
 * server has published, since otherwise this port has no version/etag check
 * and would happily run the same cached file forever. Returns true (with
 * `line` filled in) whenever there is a result to show the user; false only
 * when WiFi never connected (nothing to show, matching every other menu
 * branch's convention here). */
static bool bruce_js_appstore_launch(
    FuriPubSub* input_events, char* line, size_t line_size, bool force_update) {
    if(!bruce_js_wifi_ensure_connected(input_events)) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, BRUCE_JS_SCRIPTS_FOLDER);
    storage_common_mkdir(storage, BRUCE_JS_APPSTORE_TOOLS_DIR);
    FileInfo info;
    bool installed =
        !force_update &&
        storage_common_stat(storage, BRUCE_JS_APPSTORE_SCRIPT_PATH, &info) == FSE_OK &&
        info.size > 0;
    furi_record_close(RECORD_STORAGE);

    bool ready = installed;
    if(!installed) {
        bruce_js_draw_toast(
            force_update ? "Updating App Store..." : "Downloading App Store...",
            bruce_js_pack_swap(0xff, 0xff, 0xff),
            bruce_js_accent_color());
        ready = bruce_js_download_to_file(
            line, line_size, BRUCE_JS_APPSTORE_URL, BRUCE_JS_APPSTORE_SCRIPT_PATH);
        if(!ready) return true; /* line already holds the specific error */
    }

    bruce_js_run_file(line, line_size, BRUCE_JS_APPSTORE_SCRIPT_PATH);
    return true;
}

/* Shows the result screen and blocks until Back -- shared by the direct-
 * launch path and every menu-loop iteration below. Caller must already hold
 * gui_direct_draw_acquire(). */
static void bruce_js_show_result(FuriPubSub* input_events, const char* line) {
    FURI_LOG_I(TAG, "%s", line);
    bruce_js_draw_screen(line);

    BruceJsScreen screen = {.exit_sem = furi_semaphore_alloc(1, 0)};
    FuriPubSubSubscription* input_sub =
        furi_pubsub_subscribe(input_events, bruce_js_screen_input_callback, &screen);
    furi_semaphore_acquire(screen.exit_sem, FuriWaitForever);
    furi_pubsub_unsubscribe(input_events, input_sub);
    furi_semaphore_free(screen.exit_sem);
}

/* Menu-launch / resume-app / Archive-and-Loader-dispatch entry point.
 * `p`, when non-empty, is a .bjs/.js path (passed by the Archive browser or
 * the Loader's "Applications" browser when the user picks a matching file
 * directly -- see archive_scene_browser.c / loader_applications.c): runs
 * immediately and shows one result screen, no menu, matching how every other
 * Flipper app that's launched with a file argument behaves.
 *
 * With no path, shows a small stripped-Bruce-style menu (Add from File /
 * App Store) in a loop: Back on a RESULT screen returns to THIS menu, not
 * straight out to Flipper (an earlier version exited the whole app on any
 * Back press, which felt broken -- real Bruce's own ESC convention steps
 * back one screen at a time). Only Back on the menu itself exits to Flipper.
 * The whole loop stays in ONE direct-draw session the entire time (never
 * released until the loop ends) -- switching back and forth to Flipper's
 * own dialog system for "Add from File" used to require releasing and
 * re-acquiring direct draw for every dialog call, which is what caused both
 * the earlier "loader unlocked" flash and, after a first attempted fix, a
 * regression where the file browser got stuck blank; see
 * bruce_js_browse_files()'s own comment for the full story. */
int32_t bruce_js_app(void* p) {
    const char* arg_path = p;

    Gui* gui = furi_record_open(RECORD_GUI);
    FuriPubSub* input_events = furi_record_open(RECORD_INPUT_EVENTS);
    gui_direct_draw_acquire(gui);

    /* Lets the keyboard and display bindings (see BruceJsScriptCtx) know
     * it's safe to touch GUI/input hardware for any script run for the rest
     * of this function -- cleared again right before
     * gui_direct_draw_release() below. The "brucejs" CLI command never sets
     * this, so those bindings stay no-ops there instead of fighting over the
     * GUI with whatever else is on screen. */
    g_script_ctx.gui_active = true;
    g_script_ctx.input_events = input_events;

    if(arg_path != NULL && strlen(arg_path) > 0) {
        char line[128];
        bruce_js_run_file(line, sizeof(line), arg_path);
        bruce_js_show_result(input_events, line);
    } else {
        static const char menu_labels[3][BRUCE_JS_LIST_LABEL_LEN] = {
            "Add from File",
            "App Store",
            "Update App Store",
        };

        while(true) {
            int selection = bruce_js_list_run(input_events, "Bruce JS", menu_labels, 3);
            if(selection < 0) {
                break; /* Back on the menu itself -- exit to Flipper. */
            }

            char line[128];
            bool ok = false;
            bool have_script = false;

            if(selection == 0) {
                have_script = bruce_js_browse_files(input_events, line, sizeof(line), &ok);
            } else {
                /* App Store launch (selection 1) or a forced re-download
                 * (selection 2, "Update App Store" -- see
                 * bruce_js_appstore_launch()'s own comment for why this
                 * exists: without it, once cached, a newer version the
                 * server publishes would never be picked up). */
                have_script = bruce_js_appstore_launch(
                    input_events, line, sizeof(line), selection == 2);
            }
            UNUSED(ok);

            if(have_script) {
                bruce_js_show_result(input_events, line);
            }
            /* Loop back to the menu either way (nothing picked, or done
             * showing the result). */
        }
    }

    g_script_ctx.gui_active = false;
    gui_direct_draw_release(gui);
    furi_record_close(RECORD_INPUT_EVENTS);
    furi_record_close(RECORD_GUI);
    return 0;
}

/* CLI-only: safe to touch hardware here since this only ever runs from a
 * user-typed command, well after full boot. Takes an optional JS expression
 * as its argument (e.g. "brucejs gpio.digitalRead(4)"), so new bindings can
 * be exercised ad hoc without a rebuild; with no argument, runs the default
 * subghz sanity check. */
static void bruce_js_cli_execute(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(pipe);
    UNUSED(context);
    const char* default_script =
        "'' + (40 + 2) + ',' + subghz.isFrequencyValid(433920000) + ',' + subghz.isFrequencyValid(1)";
    const char* script =
        furi_string_size(args) > 0 ? furi_string_get_cstr(args) : default_script;
    char line[128];
    bool ok = bruce_js_run_test(line, sizeof(line), script);
    printf("%s\r\n", line);
    printf(ok ? "mquickjs engine OK\r\n" : "mquickjs engine FAILED\r\n");
}

void bruce_js_app_on_system_start(void) {
    /* NOTE: js_app.c's own CLI registration guards this same call with
     * "#ifdef SRV_CLI", following the convention that the "cli" app's own
     * application.fam sets cdefines=["SRV_CLI"] -- but that define is scoped
     * to the "cli" app's OWN compilation unit only, not the whole firmware
     * (confirmed via build_t_embed/compile_commands.json: bruce_js_app.c's
     * actual compile command has no -DSRV_CLI at all). An #ifdef SRV_CLI
     * guard here silently compiled this whole registration out on every
     * build -- "brucejs" never existed as a real command. The "cli" app is
     * unconditionally in fam_config.py's APPS list for this port, so the
     * guard serves no real purpose here; register unconditionally instead. */
    CliRegistry* registry = furi_record_open(RECORD_CLI);
    cli_registry_add_command(registry, "brucejs", CliCommandFlagDefault, bruce_js_cli_execute, NULL);
    furi_record_close(RECORD_CLI);
}
