#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    QflipperBridgePcLinkNativeSerial,
    QflipperBridgePcLinkCompositeIdle,
    QflipperBridgePcLinkQflipper,
} QflipperBridgePcLinkMode;

/**
 * Background qFlipper bridge.
 *
 * Installs the TinyUSB composite (VID/PID 0483:5740 — a real Flipper Zero) and
 * pipes a Flipper RPC session over CDC-ACM, so the desktop qFlipper app on a
 * host PC can talk to the device. Runs in its own FuriThread so the user stays
 * on the desktop while the bridge is active (toggled from the lock menu).
 *
 * Only the ESP32-S3 / S2 path has USB-OTG; on other targets start() is a no-op
 * that returns false.
 */

/** Install the composite (idempotent — reuses an already-installed one) and
 *  start the bridge thread. A second call while active returns true. */
bool qflipper_bridge_start(void);

/** Stop the bridge thread and close the RPC session, but leave the composite
 *  installed so another consumer such as USB-Storage can reuse it. */
void qflipper_bridge_stop(void);

/** True while the bridge thread is running. */
bool qflipper_bridge_is_active(void);

/** Current host-facing USB mode for the Control Centre status tile. */
QflipperBridgePcLinkMode qflipper_bridge_pc_link_mode(void);

/** Stop qFlipper and fully tear down TinyUSB, returning to native
 *  USB-Serial-JTAG in the same boot. */
bool qflipper_bridge_restore_native_serial(void);

/** Auto-off: once a host had the CDC port open (DTR up) and then dropped it for
 *  QFLIPPER_BRIDGE_AUTO_OFF_MS without coming back (qT-Embed closed, cable
 *  pulled), the bridge calls this callback from its own thread. The receiver
 *  (desktop) must then stop the bridge + uninstall the composite on ITS thread
 *  (qflipper_bridge_stop joins the bridge thread, so it can't run here). Short
 *  session restarts of qT-Embed (100-300 ms) stay well below the grace period. */
#define QFLIPPER_BRIDGE_AUTO_OFF_MS 3000
typedef void (*QflipperBridgeAutoOffCallback)(void* context);
void qflipper_bridge_set_auto_off_callback(QflipperBridgeAutoOffCallback callback, void* context);

#ifdef __cplusplus
}
#endif
