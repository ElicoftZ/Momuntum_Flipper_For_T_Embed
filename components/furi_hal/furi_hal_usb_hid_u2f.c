#include "furi_hal_usb_hid_u2f.h"

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2

#include "furi_hal_usb_hid_backend.h"
#include "furi_hal_usb_tinyusb_composite.h"
#include "furi_hal_cortex.h"
#include "furi_hal_bq25896.h"

#include <furi.h>
#include <string.h>

#include "tinyusb.h"
#include "class/hid/hid_device.h"

#define TAG "FuriHalHidU2f"

/* HID instance 1 in every descriptor this port builds. Instance 0 is the
 * keyboard/mouse/consumer collection. */
#define HID_U2F_INSTANCE 1

/* The host re-arms the OUT endpoint inside TinyUSB the moment a frame is
 * delivered, so back-to-back continuation frames can land before the worker
 * thread runs. A multi-frame U2F_AUTHENTICATE request is three frames, so a
 * single slot would drop data on a lost race. Eight is comfortably more than
 * any single request needs. */
#define HID_U2F_RX_QUEUE 8

/* tud_hid_n_report() fails while the previous IN transfer is still in flight. */
#define HID_U2F_TX_TIMEOUT_MS 100

/* USB VBUS is ~5 V when attached and collapses when not. Well clear of both. */
#define HID_U2F_VBUS_MIN_MV 4000

static const uint8_t hid_u2f_report_descriptor[] = {
    TUD_HID_REPORT_DESC_FIDO_U2F(HID_U2F_PACKET_LEN),
};

typedef struct {
    FuriMutex* mutex;
    HidU2fCallback callback;
    void* context;
    bool armed;
    bool connected;
    uint8_t queue[HID_U2F_RX_QUEUE][HID_U2F_PACKET_LEN];
    size_t head;
    size_t count;
    /* Latches once a real VBUS reading is seen; see poll_connection. */
    bool vbus_seen;
    /* Diagnostics. There is no serial console while the composite owns the USB
     * PHY -- the CDC enumerates but nothing logs to it -- so these are read off
     * the device screen instead. This rig is what found the EP5 FIFO bug, and
     * CTAP2 is a far bigger surface than U2F with no upstream to diff against. */
    FuriHalHidU2fStats stats;
} FuriHalHidU2f;

static FuriHalHidU2f hid_u2f = {0};

/* True when U2F itself installed the composite, so leaving the app knows it may
 * tear it down again. A composite someone else brought up (USB storage, the
 * qFlipper bridge) is not ours to remove. */
static bool hid_u2f_owns_composite = false;

static void hid_u2f_lock(void) {
    if(hid_u2f.mutex) furi_mutex_acquire(hid_u2f.mutex, FuriWaitForever);
}

static void hid_u2f_unlock(void) {
    if(hid_u2f.mutex) furi_mutex_release(hid_u2f.mutex);
}

const uint8_t* furi_hal_hid_u2f_report_desc(size_t* out_len) {
    if(out_len) *out_len = sizeof(hid_u2f_report_descriptor);
    return hid_u2f_report_descriptor;
}

void furi_hal_hid_u2f_init(void) {
    if(!hid_u2f.mutex) {
        hid_u2f.mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    }
}

bool furi_hal_hid_u2f_start(void) {
    furi_hal_hid_u2f_init();

    /* Bring a stack up only if nothing is running: esp_tinyusb has no working
     * uninstall, so a second tinyusb_driver_install() would fail.
     *
     * The BadUsb HID-only stack carries FIDO too and only needs two IN endpoints,
     * so it is always fine. The Composite is not: it must be installed with the
     * U2F layout, because MSC and FIDO cannot coexist on four IN endpoints. If USB
     * storage or the qFlipper bridge got there first, the layout is fixed for this
     * boot and U2F cannot run until a reboot. */
    if(!furi_hal_usb_composite_is_installed() && !furi_hal_usb_hid_backend_is_installed()) {
        if(!furi_hal_usb_composite_install(0, 0, NULL, NULL, true)) {
            FURI_LOG_E(TAG, "Unable to install a USB stack for U2F");
            return false;
        }
        hid_u2f_owns_composite = true;
    } else if(
        furi_hal_usb_composite_is_installed() && !furi_hal_usb_composite_is_u2f()) {
        FURI_LOG_E(
            TAG,
            "USB is already up as storage; reboot before using U2F");
        return false;
    }

    hid_u2f_lock();
    hid_u2f.armed = true;
    hid_u2f.head = 0;
    hid_u2f.count = 0;
    hid_u2f.connected = tud_mounted();
    hid_u2f_unlock();

    return true;
}

/* The T-Embed is battery powered, i.e. SELF-powered, and nothing on this board is
 * wired to a VBUS sense GPIO -- so tinyusb_config_t.self_powered/vbus_monitor_io
 * cannot be used. Without VBUS sensing the OTG core is never told the cable was
 * pulled: no tud_umount_cb, no suspend, and the app sits on "Connected!" forever.
 *
 * The charger IC knows, though. Poll it and synthesise the disconnect.
 *
 * Only ever acts after VBUS has actually been seen high once, so a board without
 * the charger (reading a constant 0) can never be mistaken for a permanent
 * unplug. Re-attach needs no help: TinyUSB raises tud_mount_cb by itself. */
void furi_hal_hid_u2f_poll_connection(void) {
    if(!hid_u2f.armed) return;

    const bool vbus_present = furi_hal_bq25896_get_vbus_voltage_mv() >= HID_U2F_VBUS_MIN_MV;

    hid_u2f_lock();
    if(vbus_present) hid_u2f.vbus_seen = true;
    const bool report_unplug = hid_u2f.vbus_seen && !vbus_present && hid_u2f.connected;
    hid_u2f_unlock();

    if(report_unplug) {
        FURI_LOG_I(TAG, "VBUS lost, reporting disconnect");
        furi_hal_hid_u2f_on_mount(false);
    }
}

void furi_hal_hid_u2f_stop(void) {
    hid_u2f_lock();
    hid_u2f.armed = false;
    hid_u2f.callback = NULL;
    hid_u2f.context = NULL;
    hid_u2f.head = 0;
    hid_u2f.count = 0;
    hid_u2f.vbus_seen = false;
    hid_u2f_unlock();

    /* Hand the USB port back. Without this the FIDO composite holds the OTG PHY
     * for the rest of the boot: no USB-Serial-JTAG (so no COM port to flash or
     * log over) and no way to bring up the storage layout, since MSC and FIDO
     * cannot coexist. Uninstalling routes the PHY back to USB-Serial-JTAG.
     *
     * Only ever tears down a composite U2F installed itself. */
    if(hid_u2f_owns_composite) {
        hid_u2f_owns_composite = false;
        furi_hal_usb_composite_uninstall();
    }
}

void furi_hal_hid_u2f_set_callback(HidU2fCallback callback, void* context) {
    furi_hal_hid_u2f_init();

    hid_u2f_lock();
    hid_u2f.callback = callback;
    hid_u2f.context = context;
    bool connected = hid_u2f.connected;
    hid_u2f_unlock();

    /* The app arms itself after the host has already enumerated in the common
     * case, and no further mount event is coming -- report the current state or
     * the view sits on "Connect me" forever. */
    if(callback && connected) {
        callback(HidU2fConnected, context);
    }
}

bool furi_hal_hid_u2f_is_connected(void) {
    return hid_u2f.connected;
}

uint32_t furi_hal_hid_u2f_get_request(uint8_t* data) {
    if(!data) return 0;

    uint32_t len = 0;
    hid_u2f_lock();
    if(hid_u2f.count > 0) {
        memcpy(data, hid_u2f.queue[hid_u2f.head], HID_U2F_PACKET_LEN);
        hid_u2f.head = (hid_u2f.head + 1) % HID_U2F_RX_QUEUE;
        hid_u2f.count--;
        len = HID_U2F_PACKET_LEN;
    }
    hid_u2f_unlock();

    return len;
}

void furi_hal_hid_u2f_send_response(uint8_t* data, uint8_t len) {
    if(!data) return;

    FuriHalCortexTimer timer = furi_hal_cortex_timer_get(HID_U2F_TX_TIMEOUT_MS * 1000);
    while(!tud_hid_n_ready(HID_U2F_INSTANCE)) {
        if(furi_hal_cortex_timer_is_expired(timer)) {
            FURI_LOG_W(TAG, "Timed out waiting for the IN endpoint");
            hid_u2f.stats.tx_fail++;
            return;
        }
        furi_delay_ms(1);
    }

    if(tud_hid_n_report(HID_U2F_INSTANCE, 0, data, len)) {
        /* Queued, NOT delivered -- an endpoint with no FIFO behind it accepts
         * the transfer and then never completes it. */
        hid_u2f.stats.tx_ok++;
    } else {
        FURI_LOG_W(TAG, "Frame send failed");
        hid_u2f.stats.tx_fail++;
    }
}

void furi_hal_hid_u2f_get_stats(FuriHalHidU2fStats* out) {
    if(!out) return;
    hid_u2f_lock();
    *out = hid_u2f.stats;
    out->mounted = hid_u2f.connected;
    hid_u2f_unlock();
    /* Asked outside the lock: TinyUSB state, not ours. A stuck-busy endpoint
     * reads as ep_ready 0 while mounted stays 1, which is the signature of a
     * queued IN transfer that will never complete. */
    out->ep_ready = tud_hid_n_ready(HID_U2F_INSTANCE);
}

/* Called from the TinyUSB task via furi_hal_usb_hid_tinyusb.c */
void furi_hal_hid_u2f_on_mount(bool mounted) {
    hid_u2f_lock();
    hid_u2f.connected = mounted;
    if(!mounted) {
        hid_u2f.head = 0;
        hid_u2f.count = 0;
    }
    HidU2fCallback callback = hid_u2f.armed ? hid_u2f.callback : NULL;
    void* context = hid_u2f.context;
    hid_u2f_unlock();

    if(callback) callback(mounted ? HidU2fConnected : HidU2fDisconnected, context);
}

void furi_hal_hid_u2f_on_report(const uint8_t* buffer, uint16_t bufsize) {
    if(!buffer || bufsize == 0) return;

    hid_u2f_lock();
    hid_u2f.stats.rx++;
    if(!hid_u2f.armed) {
        hid_u2f_unlock();
        return;
    }

    if(hid_u2f.count == HID_U2F_RX_QUEUE) {
        /* Nothing sensible to do but drop: the host owns the pacing and the
         * transfer is already complete. The channel times out and retries. */
        hid_u2f_unlock();
        FURI_LOG_W(TAG, "RX queue full, frame dropped");
        return;
    }

    size_t tail = (hid_u2f.head + hid_u2f.count) % HID_U2F_RX_QUEUE;
    size_t len = (bufsize < HID_U2F_PACKET_LEN) ? bufsize : HID_U2F_PACKET_LEN;
    memset(hid_u2f.queue[tail], 0, HID_U2F_PACKET_LEN);
    memcpy(hid_u2f.queue[tail], buffer, len);
    hid_u2f.count++;

    HidU2fCallback callback = hid_u2f.callback;
    void* context = hid_u2f.context;
    hid_u2f_unlock();

    if(callback) callback(HidU2fRequest, context);
}

#else /* !ESP32-S3 / S2: no USB-OTG, so no U2F transport */

#include <stddef.h>

void furi_hal_hid_u2f_init(void) {
}

bool furi_hal_hid_u2f_start(void) {
    return false;
}

void furi_hal_hid_u2f_stop(void) {
}

void furi_hal_hid_u2f_poll_connection(void) {
}

void furi_hal_hid_u2f_set_callback(HidU2fCallback callback, void* context) {
    (void)callback;
    (void)context;
}

bool furi_hal_hid_u2f_is_connected(void) {
    return false;
}

uint32_t furi_hal_hid_u2f_get_request(uint8_t* data) {
    (void)data;
    return 0;
}

void furi_hal_hid_u2f_send_response(uint8_t* data, uint8_t len) {
    (void)data;
    (void)len;
}

void furi_hal_hid_u2f_get_stats(FuriHalHidU2fStats* out) {
    if(out) memset(out, 0, sizeof(*out));
}

const uint8_t* furi_hal_hid_u2f_report_desc(size_t* out_len) {
    if(out_len) *out_len = 0;
    return NULL;
}

void furi_hal_hid_u2f_on_mount(bool mounted) {
    (void)mounted;
}

void furi_hal_hid_u2f_on_report(const uint8_t* buffer, uint16_t bufsize) {
    (void)buffer;
    (void)bufsize;
}

#endif
