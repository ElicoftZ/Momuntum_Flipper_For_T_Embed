#pragma once

// Android TV Remote (v2) client for the WiFi app's "Android TV" remote.
//
// Reimplements the `androidtvremote2` protocol (as used by the reference
// tv_keep_alive.py) natively on the ESP32. The protocol is device-agnostic:
// it drives any Android TV / Google TV box (Sony, Nvidia Shield, Xiaomi Mi
// Box, Chromecast with Google TV, ...). It provides:
//   * a one-time self-signed RSA-2048 client certificate (persisted to the SD),
//   * the Polo pairing handshake on TLS port 6467 (PIN shown on the TV),
//   * the remote-control session on TLS port 6466 (RemoteMessage protobuf,
//     ping/pong keep-alive, key injection).
//
// All blocking TLS/socket work runs on a dedicated worker task (never a
// FuriThread — lwIP sockets must be driven from an xTaskCreate task, see the
// project memory note). Scenes fire an async op and poll wlan_androidtv_state()
// from their tick handler. The remote-control session is long-lived: once
// Connected the worker keeps the socket open (answering pings) and drains a
// key queue fed by wlan_androidtv_send_key().

#include <stdbool.h>
#include <stddef.h>

#define WLAN_ATV_ERR_MAX 128
#define WLAN_ATV_NAME_MAX 48
#define WLAN_ATV_PIN_LEN 6 // 6 hex chars shown on the TV

// Android RemoteKeyCode values used by the remote UI (subset of the protocol
// enum). direction is always SHORT internally.
typedef enum {
    WlanAtvKeyPower = 26,
    WlanAtvKeyHome = 3,
    WlanAtvKeyBack = 4,
    WlanAtvKeyDpadUp = 19,
    WlanAtvKeyDpadDown = 20,
    WlanAtvKeyDpadLeft = 21,
    WlanAtvKeyDpadRight = 22,
    WlanAtvKeyDpadCenter = 23,
    WlanAtvKeyVolumeUp = 24,
    WlanAtvKeyVolumeDown = 25,
    WlanAtvKeyMute = 91,
    WlanAtvKeyMenu = 82,
    WlanAtvKeyChannelUp = 166,
    WlanAtvKeyChannelDown = 167,
    WlanAtvKeyMediaPlayPause = 85,
    WlanAtvKeyMediaNext = 87,
    WlanAtvKeyMediaPrevious = 88,
    WlanAtvKeyMediaRewind = 89,
    WlanAtvKeyMediaFastForward = 90,
    WlanAtvKeyInput = 178, // KEYCODE_TV_INPUT
} WlanAtvKey;

typedef enum {
    WlanAtvStateIdle = 0, // nothing running, no result
    WlanAtvStateBusy, // an op is running on the worker
    WlanAtvStateConnected, // remote-control session live (ready for keys)
    WlanAtvStateNeedsPair, // TCP ok but no remote session -> must pair first
    WlanAtvStatePairShowPin, // pairing started, TV shows the PIN (enter it)
    WlanAtvStatePaired, // pairing finished OK
    WlanAtvStateError, // last op failed (wlan_androidtv_error() has the message)
} WlanAtvState;

typedef struct WlanAndroidTv WlanAndroidTv;

WlanAndroidTv* wlan_androidtv_alloc(void);
void wlan_androidtv_free(WlanAndroidTv* atv);

// --- Async operations (spawn the worker; poll state afterwards) ---

/** Open the remote-control session with server ip ("a.b.c.d") using the stored
 *  client certificate. Result: Connected (ready for keys), NeedsPair (no cert
 *  yet or the TV refused the session) or Error (TV unreachable). */
void wlan_androidtv_op_connect(WlanAndroidTv* atv, const char* ip);

/** Begin the pairing handshake with server ip. Generates and persists the
 *  client certificate on first use (a few seconds). Result: PairShowPin (the
 *  TV now shows a 6-hex-char PIN — call wlan_androidtv_op_pair_finish) or
 *  Error. */
void wlan_androidtv_op_pair_start(WlanAndroidTv* atv, const char* ip);

/** Finish pairing with the 6-hex-char PIN shown on the TV. Only valid while
 *  state == PairShowPin. Result: Paired or Error. */
void wlan_androidtv_op_pair_finish(WlanAndroidTv* atv, const char* pin);

/** Queue a key to send to the TV. Only meaningful while Connected; returns
 *  immediately (the worker sends it from its session loop). */
void wlan_androidtv_send_key(WlanAndroidTv* atv, int keycode);

// --- Status / results (poll from a scene tick) ---

WlanAtvState wlan_androidtv_state(WlanAndroidTv* atv);
const char* wlan_androidtv_error(WlanAndroidTv* atv); // valid when state == Error

/** Device label ("vendor model") learned during the remote handshake; empty
 *  until Connected (or if the TV didn't report it). */
void wlan_androidtv_device_name(WlanAndroidTv* atv, char* out, size_t sz);

/** True while the remote-control session socket is alive. */
bool wlan_androidtv_is_connected(WlanAndroidTv* atv);

/** Tear down the current session/socket and stop the worker's session loop.
 *  Safe to call when idle. */
void wlan_androidtv_disconnect(WlanAndroidTv* atv);
