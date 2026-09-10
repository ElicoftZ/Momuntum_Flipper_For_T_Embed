#include "furi_hal_usb.h"
#include "furi_hal_usb_hid.h"
#include "furi_hal_usb_hid_backend.h"
#include "furi_hal_usb_hid_u2f.h"

#include <furi.h>
#include <furi_hal_version.h>
#include <string.h>
#include <stdio.h>

#include "tinyusb.h"
#include "class/hid/hid_device.h"

#define TAG "FuriHalUsbHid"

#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE    2
#define REPORT_ID_CONSUMER 3

#define HID_EP_IN       0x81
#define HID_EP_BUF_SIZE 16
#define HID_POLL_MS     5

/* FIDO U2F is a second HID interface, present here as well as in the Composite
 * descriptor, so U2F works whichever stack came up first this boot. */
#define HID_U2F_ITF_NUM     1
/* Endpoint NUMBER matters, not just the count. dfifo_alloc() writes the TX FIFO
 * register as dieptxf[epnum - 1], and the ESP32-S3's ep_in_count = 5 means EP0
 * plus only FOUR dedicated IN FIFOs -- endpoints 1..4. An IN endpoint numbered 5
 * opens without error (dcd_edpt_open bounds-checks neither the number nor the
 * FIFO register) but has no TX FIFO, so every queued transfer stalls and the
 * endpoint is busy forever. Keep every IN endpoint at 0x81..0x84. */
/* This layout only uses EP1, so EP2 is free. */
#define HID_U2F_EP_OUT      0x02
#define HID_U2F_EP_IN       0x82
#define HID_U2F_EP_BUF_SIZE 64
#define HID_U2F_POLL_MS     5

static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_ID_CONSUMER)),
};

#define HID_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

/* wDescriptorLength of the U2F interface sits 7 bytes into its 9-byte HID class
 * descriptor, which follows the interface's own 9 bytes. The U2F report
 * descriptor lives in another translation unit, so the length is written in at
 * install time -- the same trick the Composite descriptor uses. */
#define HID_U2F_WDESC_OFFSET (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + 9 + 7)

static const uint8_t hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, HID_CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(
        0,
        4,
        false,
        sizeof(hid_report_descriptor),
        HID_EP_IN,
        HID_EP_BUF_SIZE,
        HID_POLL_MS),
    TUD_HID_INOUT_DESCRIPTOR(
        HID_U2F_ITF_NUM,
        5 /* iInterface = "FIDO U2F" */,
        false,
        0 /* report_desc_len patched at install-time */,
        HID_U2F_EP_OUT,
        HID_U2F_EP_IN,
        HID_U2F_EP_BUF_SIZE,
        HID_U2F_POLL_MS),
};

static uint8_t s_config_desc_writable[sizeof(hid_configuration_descriptor)];

static tusb_desc_device_t hid_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = HID_VID_DEFAULT,
    .idProduct = HID_PID_DEFAULT,
    /* Bumped when the interface layout changes. Windows caches a device's
     * descriptors keyed on VID/PID/bcdDevice: adding the FIDO interface without
     * changing any of them leaves the host using the STALE cached layout, so the
     * new interface never appears and the device can land in an error state.
     * 0x0100 = pre-U2F, 0x0101 = FIDO interface added. */
    .bcdDevice = 0x0101,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static char s_manuf[HID_MANUF_PRODUCT_NAME_LEN + 1];
static char s_product[HID_MANUF_PRODUCT_NAME_LEN + 1];
static char s_serial[17];
static const char* s_string_descriptor[6];

typedef struct {
    bool installed;
    bool mounted;
    uint8_t led_state;
    uint8_t modifiers;
    uint8_t keys[HID_KB_MAX_KEYS];
    uint8_t mouse_buttons;
    uint16_t consumer[HID_CONSUMER_MAX_KEYS];
} HidState;

static HidState s_state = {0};
static FuriMutex* s_state_mutex = NULL;
static HidStateCallback s_user_cb = NULL;
static void* s_user_ctx = NULL;

static void hid_state_lock(void) {
    if(s_state_mutex) furi_mutex_acquire(s_state_mutex, FuriWaitForever);
}

static void hid_state_unlock(void) {
    if(s_state_mutex) furi_mutex_release(s_state_mutex);
}

static void hid_publish_mount(bool mounted) {
    s_state.mounted = mounted;
    if(!mounted) {
        memset(s_state.keys, 0, sizeof(s_state.keys));
        memset(s_state.consumer, 0, sizeof(s_state.consumer));
        s_state.modifiers = 0;
        s_state.mouse_buttons = 0;
        s_state.led_state = 0;
    }
    if(s_user_cb) s_user_cb(mounted, s_user_ctx);
}

/* TinyUSB mount callbacks - invoked from TinyUSB task */
void tud_mount_cb(void) {
    hid_state_lock();
    hid_publish_mount(true);
    hid_state_unlock();
    furi_hal_hid_u2f_on_mount(true);
}

void tud_umount_cb(void) {
    hid_state_lock();
    hid_publish_mount(false);
    hid_state_unlock();
    furi_hal_hid_u2f_on_mount(false);
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    hid_state_lock();
    hid_publish_mount(false);
    hid_state_unlock();
    furi_hal_hid_u2f_on_mount(false);
}

void tud_resume_cb(void) {
    bool mounted = tud_mounted();
    hid_state_lock();
    hid_publish_mount(mounted);
    hid_state_unlock();
    furi_hal_hid_u2f_on_mount(mounted);
}

/* TinyUSB HID callbacks */
uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    if(instance == HID_U2F_ITF_NUM) {
        return furi_hal_hid_u2f_report_desc(NULL);
    }
    return hid_report_descriptor;
}

/* Exposed for the Composite Device (HID + CDC + MSC) descriptor in
 * furi_hal_usb_tinyusb_composite.c which needs to patch wDescriptorLength. */
const uint8_t* furi_hal_usb_hid_report_desc(size_t* out_len) {
    if(out_len) *out_len = sizeof(hid_report_descriptor);
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t* buffer,
    uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t const* buffer,
    uint16_t bufsize) {
    if(instance == HID_U2F_ITF_NUM) {
        /* OUT-endpoint frames arrive here with report_id 0; this build's TinyUSB
         * passes report_type HID_REPORT_TYPE_OUTPUT (hid_device.c:410). Neither is
         * filtered on, so both that and the older INVALID convention work. */
        furi_hal_hid_u2f_on_report(buffer, bufsize);
        return;
    }
    if(report_type == HID_REPORT_TYPE_OUTPUT && report_id == REPORT_ID_KEYBOARD &&
       bufsize >= 1) {
        s_state.led_state = buffer[0];
    }
}

/* Backend start/stop - called from furi_hal_usb.c */
bool furi_hal_usb_hid_backend_start(const FuriHalUsbHidConfig* cfg) {
    if(s_state.installed) {
        /* Already installed. This happens when re-entering BadUsb after a config-
         * menu visit: leaving the work scene calls set_config(NULL) -> backend_stop,
         * which sets mounted=false, and returning calls set_config(usb_hid) ->
         * backend_start. The TinyUSB stack and the physical USB connection stay up
         * the whole time (we never tear the stack down on this port), so resync our
         * mounted flag with the real device state instead of leaving it stuck at
         * false — otherwise BadUsb shows "Connect to device" until a USB replug. */
        hid_state_lock();
        hid_publish_mount(tud_mounted());
        hid_state_unlock();
        return true;
    }

    if(!s_state_mutex) {
        s_state_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    }

    const char* manuf = (cfg && cfg->manuf[0]) ? cfg->manuf : "Flipper Devices Inc.";
    const char* product =
        (cfg && cfg->product[0]) ? cfg->product : furi_hal_version_get_name_ptr();
    if(!product || !product[0]) product = "Flipper Zero";
    uint16_t vid = (cfg && cfg->vid) ? (uint16_t)cfg->vid : HID_VID_DEFAULT;
    uint16_t pid = (cfg && cfg->pid) ? (uint16_t)cfg->pid : HID_PID_DEFAULT;

    snprintf(s_manuf, sizeof(s_manuf), "%s", manuf);
    snprintf(s_product, sizeof(s_product), "%s", product);
    snprintf(s_serial, sizeof(s_serial), "FZESP32");

    hid_device_descriptor.idVendor = vid;
    hid_device_descriptor.idProduct = pid;

    s_string_descriptor[0] = (const char[]){0x09, 0x04};
    s_string_descriptor[1] = s_manuf;
    s_string_descriptor[2] = s_product;
    s_string_descriptor[3] = s_serial;
    s_string_descriptor[4] = "HID";
    s_string_descriptor[5] = "FIDO U2F";

    memcpy(s_config_desc_writable, hid_configuration_descriptor, sizeof(s_config_desc_writable));
    size_t u2f_report_len = 0;
    furi_hal_hid_u2f_report_desc(&u2f_report_len);
    s_config_desc_writable[HID_U2F_WDESC_OFFSET] = (uint8_t)(u2f_report_len & 0xFF);
    s_config_desc_writable[HID_U2F_WDESC_OFFSET + 1] = (uint8_t)((u2f_report_len >> 8) & 0xFF);

    tinyusb_config_t tusb_cfg = {
        .device_descriptor = &hid_device_descriptor,
        .string_descriptor = s_string_descriptor,
        .string_descriptor_count =
            sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]),
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = s_config_desc_writable,
        .hs_configuration_descriptor = s_config_desc_writable,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = s_config_desc_writable,
#endif
    };

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "tinyusb_driver_install failed: %d", err);
        return false;
    }

    s_state.installed = true;
    FURI_LOG_I(TAG, "TinyUSB HID installed vid=%04x pid=%04x", vid, pid);
    return true;
}

bool furi_hal_usb_hid_backend_is_installed(void) {
    return s_state.installed;
}

void furi_hal_usb_hid_backend_stop(void) {
    /* esp_tinyusb in ESP-IDF v5 does not expose a reliable uninstall.
     * We reset state and leave the stack running; re-entry to usb_hid
     * short-circuits via the installed flag. */
    hid_state_lock();
    if(s_state.mounted) hid_publish_mount(false);
    hid_state_unlock();
}

/* Public HID API */
bool furi_hal_hid_is_connected(void) {
    return s_state.mounted;
}

uint8_t furi_hal_hid_get_led_state(void) {
    return s_state.led_state;
}

void furi_hal_hid_set_state_callback(HidStateCallback cb, void* ctx) {
    s_user_cb = cb;
    s_user_ctx = ctx;
    if(cb) cb(s_state.mounted, ctx);
}

/* Wait until the HID IN endpoint can accept a new report.
 *
 * tud_hid_*_report() only queues into the endpoint FIFO; the report is "in
 * flight" (tud_hid_ready() == false) until the host polls it (every HID_POLL_MS).
 * BadUsb fires kb_press() immediately followed by kb_release() with no delay, so
 * without this wait the release report is dropped while the press is still in
 * flight -> the host never sees key-up -> auto-repeat / garbled output. This is
 * especially visible on the composite device (HID shares the bus with CDC+MSC).
 *
 * Must be called with the state lock held; the lock is released transiently
 * while delaying so TinyUSB mount/umount callbacks can still run. Returns true
 * if the endpoint is ready to accept a report. */
static bool hid_wait_tx_ready_locked(void) {
    uint32_t timeout_ms = 100;
    while(s_state.mounted && !tud_hid_ready()) {
        if(timeout_ms-- == 0) break;
        hid_state_unlock();
        furi_delay_ms(1);
        hid_state_lock();
    }
    return s_state.mounted && tud_hid_ready();
}

static bool send_keyboard_report_locked(void) {
    if(!hid_wait_tx_ready_locked()) return false;
    return tud_hid_keyboard_report(REPORT_ID_KEYBOARD, s_state.modifiers, s_state.keys);
}

bool furi_hal_hid_kb_press(uint16_t button) {
    uint8_t keycode = button & 0xFF;
    uint8_t mods = (button >> 8) & 0xFF;

    hid_state_lock();
    s_state.modifiers |= mods;
    if(keycode) {
        bool present = false;
        for(int i = 0; i < HID_KB_MAX_KEYS; i++) {
            if(s_state.keys[i] == keycode) {
                present = true;
                break;
            }
        }
        if(!present) {
            for(int i = 0; i < HID_KB_MAX_KEYS; i++) {
                if(s_state.keys[i] == 0) {
                    s_state.keys[i] = keycode;
                    break;
                }
            }
        }
    }
    bool result = send_keyboard_report_locked();
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_kb_release(uint16_t button) {
    uint8_t keycode = button & 0xFF;
    uint8_t mods = (button >> 8) & 0xFF;

    hid_state_lock();
    s_state.modifiers &= ~mods;
    if(keycode) {
        uint8_t compact[HID_KB_MAX_KEYS] = {0};
        int idx = 0;
        for(int i = 0; i < HID_KB_MAX_KEYS; i++) {
            if(s_state.keys[i] && s_state.keys[i] != keycode) {
                compact[idx++] = s_state.keys[i];
            }
        }
        memcpy(s_state.keys, compact, sizeof(s_state.keys));
    }
    bool result = send_keyboard_report_locked();
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_kb_release_all(void) {
    hid_state_lock();
    s_state.modifiers = 0;
    memset(s_state.keys, 0, sizeof(s_state.keys));
    bool result = send_keyboard_report_locked();
    hid_state_unlock();
    return result;
}

static bool send_mouse_report_locked(int8_t dx, int8_t dy, int8_t scroll) {
    if(!hid_wait_tx_ready_locked()) return false;
    return tud_hid_mouse_report(
        REPORT_ID_MOUSE, s_state.mouse_buttons, dx, dy, scroll, 0);
}

bool furi_hal_hid_mouse_move(int8_t dx, int8_t dy) {
    hid_state_lock();
    bool result = send_mouse_report_locked(dx, dy, 0);
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_mouse_press(uint8_t button) {
    hid_state_lock();
    s_state.mouse_buttons |= button;
    bool result = send_mouse_report_locked(0, 0, 0);
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_mouse_release(uint8_t button) {
    hid_state_lock();
    s_state.mouse_buttons &= ~button;
    bool result = send_mouse_report_locked(0, 0, 0);
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_mouse_scroll(int8_t delta) {
    hid_state_lock();
    bool result = send_mouse_report_locked(0, 0, delta);
    hid_state_unlock();
    return result;
}

static bool send_consumer_report_locked(void) {
    if(!hid_wait_tx_ready_locked()) return false;
    /* Standard TUD_HID_REPORT_DESC_CONSUMER emits one 16-bit usage.
     * We pick the most-recently pressed key that is still active. */
    uint16_t usage = 0;
    for(int i = HID_CONSUMER_MAX_KEYS - 1; i >= 0; i--) {
        if(s_state.consumer[i]) {
            usage = s_state.consumer[i];
            break;
        }
    }
    return tud_hid_report(REPORT_ID_CONSUMER, &usage, sizeof(usage));
}

bool furi_hal_hid_consumer_key_press(uint16_t button) {
    hid_state_lock();
    bool already = false;
    for(int i = 0; i < HID_CONSUMER_MAX_KEYS; i++) {
        if(s_state.consumer[i] == button) {
            already = true;
            break;
        }
    }
    if(!already) {
        for(int i = 0; i < HID_CONSUMER_MAX_KEYS; i++) {
            if(s_state.consumer[i] == 0) {
                s_state.consumer[i] = button;
                break;
            }
        }
    }
    bool result = send_consumer_report_locked();
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_consumer_key_release(uint16_t button) {
    hid_state_lock();
    for(int i = 0; i < HID_CONSUMER_MAX_KEYS; i++) {
        if(s_state.consumer[i] == button) s_state.consumer[i] = 0;
    }
    bool result = send_consumer_report_locked();
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_consumer_key_release_all(void) {
    hid_state_lock();
    memset(s_state.consumer, 0, sizeof(s_state.consumer));
    bool result = send_consumer_report_locked();
    hid_state_unlock();
    return result;
}
