#include "furi_hal_usb_tinyusb_composite.h"

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2

#include <furi.h>
#include <furi_hal_version.h>
#include <string.h>
#include <stdio.h>

#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "class/hid/hid_device.h"
#include "furi_hal_usb_hid_u2f.h"
#include "class/msc/msc_device.h"

/* Low-level access to switch the internal USB FSLS PHY mux between the OTG
 * controller and the USB-Serial-JTAG controller (see composite_uninstall). */
#include "hal/usb_serial_jtag_ll.h"

#define TAG "FuriHalUsbComp"

/* Default identity if backend doesn't supply one. */
/* Masquerade as a real Flipper Zero so qFlipper (and other host tools that
 * filter strictly on VID/PID) detect the device. ESP32-S3 native USB-OTG lets
 * us choose any descriptor identity. */
#define COMP_VID_DEFAULT 0x0483 /* STMicroelectronics (real Flipper Zero) */
#define COMP_PID_DEFAULT 0x5740 /* CDC / Virtual ComPort */

/* HID Report IDs - shared with furi_hal_usb_hid_tinyusb.c via convention */
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE    2
#define REPORT_ID_CONSUMER 3

/* Endpoint layout */
#define HID_EP_IN       0x81
#define HID_EP_BUF_SIZE 16
#define HID_POLL_MS     5

#define CDC_EP_NOTIF      0x82
#define CDC_EP_NOTIF_SIZE 8
#define CDC_EP_OUT        0x03
#define CDC_EP_IN         0x83
#define CDC_EP_BUF_SIZE   64

#define MSC_EP_OUT      0x04
#define MSC_EP_IN       0x84
#define MSC_EP_BUF_SIZE 64

/* Endpoint NUMBER matters, not just the count. dfifo_alloc() writes the TX FIFO
 * register as dieptxf[epnum - 1], and the ESP32-S3's ep_in_count = 5 means EP0
 * plus only FOUR dedicated IN FIFOs -- endpoints 1..4. An IN endpoint numbered 5
 * opens without error (dcd_edpt_open bounds-checks neither the number nor the
 * FIFO register) but has no TX FIFO, so every queued transfer stalls and the
 * endpoint is busy forever. Keep every IN endpoint at 0x81..0x84. */
/* Reuses MSC's numbers, which is safe because MSC is absent from this layout. */
#define HID_U2F_EP_OUT      0x04
#define HID_U2F_EP_IN       0x84
#define HID_U2F_EP_BUF_SIZE 64
#define HID_U2F_POLL_MS     5

/* THE ESP32-S3 HAS ONLY FOUR USABLE IN ENDPOINTS, so MSC and FIDO cannot both be
 * present. dwc2_esp32.h declares ep_in_count = 5, and dcd_dwc2.c allocates EP0 IN
 * through the same counter (dfifo_device_init -> dfifo_alloc(0x80)), so the
 * TU_ASSERT(allocated_epin_count < 5) budget is EP0 plus four.
 *
 * HID + CDC-notify + CDC-data already spend three of those four. Adding both MSC
 * and FIDO asks for five, dcd_edpt_open() fails, the device never reaches the
 * configured state, and the host cannot enumerate it -- which looks exactly like
 * 'Windows does not recognise the device' while the U2F app sits on 'Connect me!'.
 *
 * So there are two layouts, chosen at install time. Both are four interfaces and
 * exactly at the endpoint budget. A full TinyUSB teardown is required before
 * changing layouts. */
#define ITF_NUM_HID       0
#define ITF_NUM_CDC_NOTIF 1
#define ITF_NUM_CDC_DATA  2
/* Interface 3 is MSC in the standard layout, FIDO in the U2F layout. */
#define ITF_NUM_LAST      3
#define ITF_TOTAL         4

#define COMP_CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

#define COMP_U2F_CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

/* wDescriptorLength sits 7 bytes into a HID class descriptor, which follows the
 * interface's own 9 bytes. Patched at install time because the report descriptors
 * live in other translation units. */
#define COMP_HID_WDESC_OFFSET (TUD_CONFIG_DESC_LEN + 9 + 7)
#define COMP_U2F_WDESC_OFFSET \
    (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN + 9 + 7)

/* Forward-declared from furi_hal_usb_hid_tinyusb.c */
extern const uint8_t* furi_hal_usb_hid_report_desc(size_t* out_len);

/* Bridge from tinyusb_cdcacm events to user-supplied CdcCallbacks.
 * Implemented in furi_hal_usb_cdc.c. */
extern void furi_hal_cdc_internal_register_tinyusb_callbacks(void);

static const uint8_t s_composite_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        ITF_TOTAL,
        0,
        COMP_CONFIG_TOTAL_LEN,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
        100),
    /* HID */
    TUD_HID_DESCRIPTOR(
        ITF_NUM_HID,
        4 /* iInterface = "HID" */,
        false,
        0 /* report_desc_len patched at install-time */,
        HID_EP_IN,
        HID_EP_BUF_SIZE,
        HID_POLL_MS),
    /* CDC */
    TUD_CDC_DESCRIPTOR(
        ITF_NUM_CDC_NOTIF,
        5 /* iInterface = "Flipper Serial" */,
        CDC_EP_NOTIF,
        CDC_EP_NOTIF_SIZE,
        CDC_EP_OUT,
        CDC_EP_IN,
        CDC_EP_BUF_SIZE),
    /* MSC */
    TUD_MSC_DESCRIPTOR(
        ITF_NUM_LAST,
        6 /* iInterface = "Flipper Storage" */,
        MSC_EP_OUT,
        MSC_EP_IN,
        MSC_EP_BUF_SIZE),
};

/* Same as above with FIDO in place of MSC. Storage and U2F are mutually exclusive
 * for the reason spelled out at the top of the interface list. */
static const uint8_t s_composite_u2f_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        ITF_TOTAL,
        0,
        COMP_U2F_CONFIG_TOTAL_LEN,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
        100),
    /* HID */
    TUD_HID_DESCRIPTOR(
        ITF_NUM_HID,
        4 /* iInterface = "HID" */,
        false,
        0 /* report_desc_len patched at install-time */,
        HID_EP_IN,
        HID_EP_BUF_SIZE,
        HID_POLL_MS),
    /* CDC */
    TUD_CDC_DESCRIPTOR(
        ITF_NUM_CDC_NOTIF,
        5 /* iInterface = "Flipper Serial" */,
        CDC_EP_NOTIF,
        CDC_EP_NOTIF_SIZE,
        CDC_EP_OUT,
        CDC_EP_IN,
        CDC_EP_BUF_SIZE),
    /* FIDO U2F */
    TUD_HID_INOUT_DESCRIPTOR(
        ITF_NUM_LAST,
        7 /* iInterface = "FIDO U2F" */,
        false,
        0 /* report_desc_len patched at install-time */,
        HID_U2F_EP_OUT,
        HID_U2F_EP_IN,
        HID_U2F_EP_BUF_SIZE,
        HID_U2F_POLL_MS),
};

_Static_assert(
    sizeof(s_composite_config_desc) == COMP_CONFIG_TOTAL_LEN,
    "standard composite descriptor length mismatch");
_Static_assert(
    sizeof(s_composite_u2f_config_desc) == COMP_U2F_CONFIG_TOTAL_LEN,
    "u2f composite descriptor length mismatch");

static uint8_t s_config_desc_writable[sizeof(s_composite_u2f_config_desc) >
                                              sizeof(s_composite_config_desc) ?
                                          sizeof(s_composite_u2f_config_desc) :
                                          sizeof(s_composite_config_desc)];
static bool s_installed_with_u2f = false;

static tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* IAD device class - required for HID + CDC composite */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = COMP_VID_DEFAULT,
    .idProduct = COMP_PID_DEFAULT,
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

static char s_manuf[64];
static char s_product[64];
static char s_serial[17];
static const char* s_string_descriptor[8];

static bool s_installed = false;

bool furi_hal_usb_composite_is_installed(void) {
    return s_installed;
}

bool furi_hal_usb_composite_is_u2f(void) {
    return s_installed && s_installed_with_u2f;
}

bool furi_hal_usb_composite_install(
    uint16_t vid,
    uint16_t pid,
    const char* manuf,
    const char* product,
    bool with_u2f) {
    FURI_LOG_I(TAG, "composite_install entered: vid=%04x pid=%04x s_installed=%d", vid, pid, (int)s_installed);
    if(s_installed) {
        if(with_u2f != s_installed_with_u2f) {
            /* One-shot install, and MSC and FIDO cannot coexist -- see the
             * interface list. Whoever got here first owns the layout. */
            FURI_LOG_E(
                TAG,
                "USB already up as %s; %s needs a reboot",
                s_installed_with_u2f ? "U2F" : "storage",
                with_u2f ? "U2F" : "storage");
            return false;
        }
        /* Identity changes after first install need a reboot to take effect. */
        if((vid && vid != s_device_descriptor.idVendor) ||
           (pid && pid != s_device_descriptor.idProduct)) {
            FURI_LOG_W(
                TAG,
                "VID/PID change ignored (composite already installed). Reboot to apply.");
        }
        return true;
    }

    /* Strings */
    const char* manuf_str = (manuf && manuf[0]) ? manuf : "Flipper Devices Inc.";
    const char* product_str = (product && product[0]) ? product : furi_hal_version_get_name_ptr();
    if(!product_str || !product_str[0]) product_str = "Flipper Zero";
    snprintf(s_manuf, sizeof(s_manuf), "%s", manuf_str);
    snprintf(s_product, sizeof(s_product), "%s", product_str);
    snprintf(s_serial, sizeof(s_serial), "FZESP32");

    s_string_descriptor[0] = (const char[]){0x09, 0x04};
    s_string_descriptor[1] = s_manuf;
    s_string_descriptor[2] = s_product;
    s_string_descriptor[3] = s_serial;
    s_string_descriptor[4] = "Flipper HID";
    s_string_descriptor[5] = "Flipper Serial";
    s_string_descriptor[6] = "Flipper Storage";
    s_string_descriptor[7] = "FIDO U2F";

    /* VID/PID */
    if(vid) s_device_descriptor.idVendor = vid;
    if(pid) s_device_descriptor.idProduct = pid;

    /* Patch the HID descriptor length (offset 16 inside TUD_HID_DESCRIPTOR within
     * the config blob: TUD_CONFIG (9 bytes) + TUD_HID up to wDescriptorLength.
     * TUD_HID_DESCRIPTOR layout is 9-byte interface + 9-byte hid + 7-byte EP.
     * wDescriptorLength is inside the 9-byte HID class descriptor at offset 7..8.
     * Absolute: 9 (config) + 9 (interface) + 7 (in HID class desc up to wDesc) = 25.
     */
    const uint8_t* source = with_u2f ? s_composite_u2f_config_desc : s_composite_config_desc;
    const size_t source_len = with_u2f ? sizeof(s_composite_u2f_config_desc) :
                                         sizeof(s_composite_config_desc);
    memcpy(s_config_desc_writable, source, source_len);

    size_t hid_report_len = 0;
    furi_hal_usb_hid_report_desc(&hid_report_len);
    s_config_desc_writable[COMP_HID_WDESC_OFFSET] = (uint8_t)(hid_report_len & 0xFF);
    s_config_desc_writable[COMP_HID_WDESC_OFFSET + 1] =
        (uint8_t)((hid_report_len >> 8) & 0xFF);

    if(with_u2f) {
        size_t u2f_report_len = 0;
        furi_hal_hid_u2f_report_desc(&u2f_report_len);
        s_config_desc_writable[COMP_U2F_WDESC_OFFSET] = (uint8_t)(u2f_report_len & 0xFF);
        s_config_desc_writable[COMP_U2F_WDESC_OFFSET + 1] =
            (uint8_t)((u2f_report_len >> 8) & 0xFF);
    }

    tinyusb_config_t tusb_cfg = {
        .device_descriptor = &s_device_descriptor,
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

    /* CDC-ACM driver init for ITF 0 (logical CDC index, mapping to TinyUSB
     * interfaces ITF_NUM_CDC_NOTIF/DATA). */
    tinyusb_config_cdcacm_t cdc_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    err = tusb_cdc_acm_init(&cdc_cfg);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "tusb_cdc_acm_init failed: %d", err);
        return false;
    }

    /* Wire CDC events into furi_hal_cdc state. */
    furi_hal_cdc_internal_register_tinyusb_callbacks();

    s_installed = true;
    s_installed_with_u2f = with_u2f;
    FURI_LOG_I(
        TAG,
        "TinyUSB Composite installed: vid=%04x pid=%04x (HID + CDC + %s)",
        s_device_descriptor.idVendor,
        s_device_descriptor.idProduct,
        with_u2f ? "FIDO" : "MSC");
    return true;
}

bool furi_hal_usb_composite_uninstall(void) {
    if(!s_installed) return true;

    /* 1) Tear down the TinyUSB device task, class drivers, queues and OTG PHY.
     *    Our esp_tinyusb compatibility hook maps tusb_teardown() to the bundled
     *    tud_deinit(), so a later install starts from clean same-boot state. */
    esp_err_t err = tinyusb_driver_uninstall();
    bool success = (err == ESP_OK);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "tinyusb_driver_uninstall failed: %d", err);
        /* Fall through and try the PHY switch anyway — partial teardown still
         * left the OTG controller idle. */
    }

    /* esp_tinyusb keeps a CDC-ACM wrapper object outside the TinyUSB core.
     * Release it too or the next same-boot tusb_cdc_acm_init() reports that
     * interface 0 is already initialized. The USB task and interrupts are
     * stopped above, so no CDC callback can race this free. */
    err = tusb_cdc_acm_deinit(TINYUSB_CDC_ACM_0);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "tusb_cdc_acm_deinit failed: %d", err);
        success = false;
    }

    /* Give the host a real disconnect interval before the same pins announce
     * the native USB-Serial-JTAG controller with a different identity. */
    furi_delay_ms(100);

    /* 2) Re-route the shared internal FSLS PHY from the OTG controller back to
     *    the USB-Serial-JTAG controller and re-enable its pads. The USJ bus
     *    clock has been running since boot (it was the console/flash port);
     *    OTG install never touched it. Re-applying the USJ pull-up makes the
     *    host re-enumerate the JTAG/serial device, so esptool can flash again.
     *
     *    usb_serial_jtag_ll_phy_enable_external(false) sets:
     *      USB_SERIAL_JTAG.conf0.phy_sel = 0
     *      RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1   (software mux control)
     *      RTCCNTL.usb_conf.sw_usb_phy_sel   = 0   (internal PHY -> USJ) */
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);

    s_installed = false;
    /* Forget the layout too, or a later install of the other one is refused
     * as a mismatch against a composite that is no longer running. */
    s_installed_with_u2f = false;
    FURI_LOG_I(TAG, "Composite uninstalled, USB-Serial-JTAG restored");
    return success;
}

void furi_hal_usb_composite_restore_serial_jtag(void) {
    /* usb_serial_jtag_ll_phy_enable_external(false) sets:
     *   USB_SERIAL_JTAG.conf0.phy_sel = 0
     *   RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1   (software mux control)
     *   RTCCNTL.usb_conf.sw_usb_phy_sel   = 0   (internal FSLS PHY -> USJ)
     * These RTC-domain bits persist across a soft reset, so calling this at
     * boot undoes a prior OTG-composite mux even after a plain reboot. */
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
}

size_t furi_hal_usb_serial_jtag_read(uint8_t* buf, size_t len) {
    if(s_installed || !buf || !len) return 0;
    if(!usb_serial_jtag_ll_rxfifo_data_available()) return 0;
    return usb_serial_jtag_ll_read_rxfifo(buf, (uint32_t)len);
}

size_t furi_hal_usb_serial_jtag_write(const uint8_t* buf, size_t len) {
    if(s_installed || !buf || !len) return 0;
    if(!usb_serial_jtag_ll_txfifo_writable()) return 0;
    size_t n = usb_serial_jtag_ll_write_txfifo(buf, (uint32_t)len);
    usb_serial_jtag_ll_txfifo_flush();
    return n;
}

#else /* !ESP32-S3 / S2 */

bool furi_hal_usb_composite_install(
    uint16_t vid,
    uint16_t pid,
    const char* manuf,
    const char* product,
    bool with_u2f) {
    (void)vid;
    (void)pid;
    (void)manuf;
    (void)product;
    (void)with_u2f;
    return false;
}

bool furi_hal_usb_composite_is_u2f(void) {
    return false;
}

bool furi_hal_usb_composite_is_installed(void) {
    return false;
}

bool furi_hal_usb_composite_uninstall(void) {
    return false;
}

void furi_hal_usb_composite_restore_serial_jtag(void) {
}

size_t furi_hal_usb_serial_jtag_read(uint8_t* buf, size_t len) {
    (void)buf;
    (void)len;
    return 0;
}

size_t furi_hal_usb_serial_jtag_write(const uint8_t* buf, size_t len) {
    (void)buf;
    (void)len;
    return 0;
}

#endif
