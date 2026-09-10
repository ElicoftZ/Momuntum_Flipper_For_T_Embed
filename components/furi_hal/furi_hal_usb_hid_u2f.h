#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* U2F HID transport (FIDO U2FHID, usage page 0xF1D0).
 *
 * The FIDO interface is HID instance 1 in every descriptor this port installs,
 * so it is reachable whichever USB stack came up first -- the Composite Device
 * (HID + CDC + MSC) or the HID-only one BadUsb brings up. Selecting
 * usb_hid_u2f through furi_hal_usb_set_config() installs a stack if none is
 * running yet and arms this module; it never tears an existing one down,
 * because esp_tinyusb cannot reliably reinstall. */

/* Fixed by the U2FHID spec: every frame is exactly one 64-byte report. */
#define HID_U2F_PACKET_LEN 64

typedef enum {
    HidU2fDisconnected,
    HidU2fConnected,
    HidU2fRequest,
} HidU2fEvent;

typedef void (*HidU2fCallback)(HidU2fEvent ev, void* context);

/* Transport counters for the on-screen diagnostics. The USB composite owns the
 * PHY for the whole boot, so there is no serial console to log to while any of
 * this is happening -- the device screen is the only instrument available. */
typedef struct {
    uint32_t rx;      /* frames handed up by TinyUSB      */
    uint32_t tx_ok;   /* frames QUEUED (not delivered)    */
    uint32_t tx_fail; /* endpoint never became ready      */
    bool mounted;     /* host has configured the device   */
    bool ep_ready;    /* IN endpoint idle, not stuck busy */
} FuriHalHidU2fStats;

void furi_hal_hid_u2f_get_stats(FuriHalHidU2fStats* out);

/** Called once from furi_hal_usb_init(). */
void furi_hal_hid_u2f_init(void);

/** Arm the U2F path, bringing a USB stack up if none is running. */
bool furi_hal_hid_u2f_start(void);

/** Disarm: drops the callback and any queued frames. The stack stays up. */
void furi_hal_hid_u2f_stop(void);

void furi_hal_hid_u2f_set_callback(HidU2fCallback callback, void* context);

bool furi_hal_hid_u2f_is_connected(void);

/* Call periodically while U2F is open. Detects an unplug that TinyUSB cannot
 * see on a self-powered board with no VBUS sense pin. */
void furi_hal_hid_u2f_poll_connection(void);

/** Pop one received frame into `data` (HID_U2F_PACKET_LEN bytes).
 * Returns the frame length, or 0 when nothing is queued. Frames arrive faster
 * than a single thread flag can signal, so drain this in a loop. */
uint32_t furi_hal_hid_u2f_get_request(uint8_t* data);

void furi_hal_hid_u2f_send_response(uint8_t* data, uint8_t len);

/* Report descriptor, shared with the descriptor builders in
 * furi_hal_usb_tinyusb_composite.c and furi_hal_usb_hid_tinyusb.c. */
const uint8_t* furi_hal_hid_u2f_report_desc(size_t* out_len);

/* Called by the TinyUSB callbacks in furi_hal_usb_hid_tinyusb.c. */
void furi_hal_hid_u2f_on_mount(bool mounted);
void furi_hal_hid_u2f_on_report(const uint8_t* buffer, uint16_t bufsize);

#ifdef __cplusplus
}
#endif
