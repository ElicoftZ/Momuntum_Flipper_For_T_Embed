# BLE serial ↔ Flipper Mobile — connection stability & pairing (handoff for Claude)

> **2026-09-13 investigation update — hardware pairing confirmed**
>
> The DisplayOnly + SC test negotiated 45 ms / 4 s connection parameters,
> opened RPC, and displayed the same passkey injected into NimBLE. After the
> user entered it, no ENC_CHANGE or disconnect arrived. The IDF SM timeout is
> fixed at 30,000 ms (`ble_sm.c`), longer than the reported app timeout.
>
> A concrete host deadlock was found: SUBSCRIBE sends Flow/RPC notifications
> while holding `serial_state.mutex`; `ble_gatts_notify_custom()` synchronously
> calls `ble_gap_notify_tx_event()`, re-entering `serial_gap_event()` and taking
> the same non-recursive mutex. Ignore non-indication NOTIFY_TX before locking.
> A host regression reproduces the old lock re-entry and passes with this fix,
> while preserving indication callbacks. PASSKEY_ACTION now releases the global
> lock before UI/SM callbacks and logs injection results. Unsupported actions,
> including INPUT (neither advertised capability has a keyboard), disconnect
> explicitly rather than injecting a random passkey.
>
> Hardware confirmed encryption, bonded reconnection, and working screen/remote control.
> DisplayOnly + SC is retained. Logs still showed a later storage-transfer failure;
> storage sync controls are a separate follow-up feature.
> Numeric Comparison remains a fallback hypothesis; earlier claims below that
> Flipper Mobile cannot support it are unverified, not established facts.
>
> **Build/flash override:** use `build_multiboot` ONLY, with
> `CONFIG_MOMENTUM_MULTIBOOT=y`, ESP-IDF v5.4.1 and Python 3.11. Flash app-only
> to BOTH `0x20000` and `0x520000`. Never flash bootloader, partition table or
> otadata. The older `build_t_embed_release` commands below are obsolete.
> Forget the device in phone Bluetooth settings before retesting.


Read this before touching `components/ble_serial/` or
`components/ble_profile/extra_profiles/serial_profile.c`.
It records the reported symptom, the architecture, exactly what was changed,
how to diagnose a remaining failure from the serial log, and what not to break.

## Symptom being fixed

User (LilyGo T-Embed CC1101, ESP32-S3) connects from the **Flipper Mobile** app:

> "When I entered pair and loading but Flipper Mobile showed failed connection."

So the failure is at the **pairing / post-connect** stage, not (only) link
quality. Two independent problems were addressed:

1. The link never negotiated connection parameters (random drops under Wi-Fi/BLE
   coexistence).
2. The serial profile used **legacy pairing + DisplayYesNo**, which can
   negotiate a flow the app has no UI for, surfacing as "failed connection".

## How the Flipper Mobile link works on this port

The port implements the **Flipper Zero serial GATT service** in plain ESP-IDF
NimBLE, so the stock Flipper mobile app can talk to it. No Flipper firmware code
is used; only the wire protocol/UUIDs are matched.

Service/characteristic UUIDs (`components/ble_serial/ble_serial.c`), already
correct — do not change:

| Role | UUID |
| --- | --- |
| Service | `8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000` |
| TX (device→phone, INDICATE) | `19ed82ae-ed21-4c9d-4145-228e61fe0000` |
| RX (phone→device, WRITE) | `19ed82ae-ed21-4c9d-4145-228e62fe0000` |
| Flow (NOTIFY) | `19ed82ae-ed21-4c9d-4145-228e63fe0000` |
| RPC status (WRITE/NOTIFY) | `19ed82ae-ed21-4c9d-4145-228e64fe0000` |

Plus Device Information `0x180A`, Battery `0x180F`, Current Time `0x1805`.
Advertising: Flags + 16-bit service `0x3080` (color bits) + name `Flipper <name>`.

Flow: phone connects → device initiates security → phone/device pair → device
sends Flow + RPC status, starts a CTS client read → phone opens the Flipper RPC
(protobuf) session over RX/TX. `bt.c` (`applications/services/bt/bt_service/`)
owns the RPC session; `furi_hal_bt.c` bridges GAP events to it.

## Files involved

- `components/ble_serial/ble_serial.c` — the NimBLE serial service (GAP events,
  security, indications, flow control). **Changed this pass.**
- `components/ble_profile/extra_profiles/serial_profile.c` — wraps `ble_serial`
  as the default `FlipperHalBleProfileTemplate`. **Changed this pass.**
- `components/nimble_glue/nimble_glue.c` — NimBLE host lifecycle, security
  defaults, NVS bond store, address selection. Not changed.
- `components/ble_hid/ble_hid.c` — the *working* reference path. The HID profile
  pairs successfully on the same hardware/phone; use it as the comparison.
- `applications/services/bt/bt_service/bt.c` — RPC session + PIN UI.

## Changes made this pass

### 1. Connection parameter negotiation (was completely missing)

`ble_serial.c`: added `serial_request_conn_params()` and call it on
`BLE_GAP_EVENT_CONNECT` (before `ble_gap_security_initiate`):

```c
.itvl_min = 0x06,               /* 7.5 ms  */
.itvl_max = 0x24,               /* 45 ms   */
.latency = 0,
.supervision_timeout = 0x0190,  /* 4 s     */
```

Rationale: as a peripheral the port accepted whatever the phone chose. Under
`CONFIG_SW_COEXIST_ENABLE=y`, a short supervision timeout / odd interval is the
classic cause of random drops and failed reconnects. Requesting this range lets
the phone pick something the coexisting radio can hold.

### 2. Secure Connections + DisplayOnly pairing (matches Flipper Mobile)

- `serial_profile.c`: default `pairing_method` changed
  `GapPairingPinCodeVerifyYesNo` → `GapPairingPinCodeDisplayOnly`.
- `ble_serial.c`: 
imble_glue_configure_security(config->bonding, true, false, io_cap)`
  → `..., true, io_cap)` (last bool is `secure_connections`).

`DisplayOnly + SC` = **Passkey Entry**: the device shows a 6-digit code and the
app asks the user to type it — the flow Flipper Mobile implements.
`DisplayYesNo + SC` can negotiate **Numeric Comparison**, which the app has no UI
for. (The HID profile uses `DisplayYesNo + SC` and works, but its host is the OS
Bluetooth UI, not the Flipper app.)

### 3. Diagnostics

- `BLE_GAP_EVENT_CONN_UPDATE`: logs the negotiated `itvl/latency/timeout`.
- `BLE_GAP_EVENT_ENC_CHANGE`: logs `Encryption established`, or
  `Encryption/pairing failed, status=N (0xN)`.
- `BLE_GAP_EVENT_PASSKEY_ACTION`: logs `Pairing passkey: NNNNNN`,
  numeric-comparison value, or `Unhandled pairing action: N`.

## Diagnosing a remaining failure from the serial log

Flash, open a serial monitor at 115200, then pair. Look for:

| Log line | Meaning / action |
| --- | --- |
| `Requested connection parameters ...` | normal |
| `Connection parameters: itvl=... timeout=...` | phone's accepted values; if `timeout` < ~100 (1 s) the phone ignored us |
| `Pairing passkey: NNNNNN` | app should prompt for this code |
| `Encryption established` | pairing succeeded; a later failure is RPC, not security |
| `Encryption/pairing failed, status=N (0xN)` | security failed — decode below |
| `Unhandled pairing action: N` | IO-capability mismatch; try a different `pairing_method` |

Common HCI status codes in `Encryption/pairing failed`:

- `0x05` Authentication Failure — MITM/IO-cap mismatch.
- `0x06` PIN or Key Missing — stale bond; forget device and re-pair.
- `0x08` Connection Timeout (supervision) — link dropped mid-pairing; connection
  params fix should help.
- `0x13` Remote User Terminated — phone/app gave up.
- `0x3E` Failed to Establish — phone rejected the connection.

## Build & flash

Incremental firmware build (verified working):

```powershell
$env:IDF_PATH = 'C:\Espressif\frameworks\esp-idf-v5.4.1'
$env:IDF_TOOLS_PATH = 'C:\Espressif'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\python_env\idf5.4_py3.12_env'
$env:ESP_ROM_ELF_DIR = 'C:\Espressif\tools\esp-rom-elfs\20241011'
$tc = 'C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin'
$env:Path = "$tc;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\python_env\idf5.4_py3.12_env\Scripts;$env:Path"
& 'C:\Espressif\tools\ninja\1.12.1\ninja.exe' -C build_t_embed_release -j 8
```

Flash (device on COM4, app at `0x20000`):

```
python .../esptool.py --chip esp32s3 --port COM4 --baud 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x20000 build_t_embed_release/furi_esp32.bin
```

**After flashing, forget the Flipper on the phone first** (Settings → Bluetooth),
then pair fresh. The pairing type changed (legacy → SC), so a cached bond can
make the app fail even with the fix. The port deletes stale peers on
`BLE_GAP_EVENT_REPEAT_PAIRING`, but the phone side must be cleared too.

## Relevant sdkconfig facts (already set)

- `CONFIG_PM_ENABLE` is **not set** → no light sleep, so the link is not dropped
  by power management.
- `CONFIG_SW_COEXIST_ENABLE=y` → Wi-Fi/BLE coexistence is the main external
  stability factor. **Do not run Wi-Fi scan/wardriving while connected.**
- `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517`, port requests `486+3=489`.
- `CONFIG_BT_NIMBLE_MSYS1_BLOCK_COUNT=12` — if large RPC transfers stall/drop,
  this pool may be exhausted; raising it costs internal RAM (audit first).
- `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`, `MAX_BONDS=8`, `NVS_PERSIST=y`.
- Both `CONFIG_BT_NIMBLE_SM_LEGACY=y` and `SM_SC=y`; `SM_SC_ONLY=0` (SC falls
  back to legacy if the peer lacks SC).

## Remaining hypotheses / next steps

1. **Confirm the pairing fix on-device.** The change is reasoned but not yet
   verified on hardware. The new log lines make the outcome unambiguous.
2. If it still fails with `status=0x05`, try `DisplayYesNo` again but with SC
   (i.e. revert only the `serial_profile.c` pairing method, keep SC=true) to see
   whether the phone prefers Numeric Comparison.
3. If it fails with `status=0x06`, it is a bond-cache problem: erase bonds on
   both sides (
imble_glue_remove_all_bonds()` exists; UI path is the BT
   "forget bonded devices" message in `bt.c`).
4. If pairing succeeds but the app still shows "failed connection", the problem
   is the **RPC handshake**, not security — inspect `bt.c`
   (`bt_open_rpc_connection` / `rpc_session_feed`) and `serial_send_value()`
   indication flow.
5. If drops happen only while Wi-Fi is active, treat it as coexistence and keep
   Wi-Fi off during the Flipper Mobile session.

## Do-not-break list

- Do **not** change the serial UUIDs or characteristic flags; they match Flipper.
- Do **not** switch the serial profile back to a random MAC; the default leaves
  `mac_address` zeroed so the **public** address is used, which the app caches.
- Keep `bonding = true` and `mitm = true`.
- The HID profile is the known-good pairing reference; if you change shared
  security code, re-verify HID still pairs.
- `ble_serial_alloc()` maps `BleSerialPairingPinCodeVerifyYesNo` → `DISPLAY_YESNO`
  and everything else → `DISPLAY_ONLY`; keep `GapPairing` (in
  `profile_interface.h`) and `BleSerialPairingMode` (in `ble_serial.h`) numeric
  values aligned.

## "Is there an easier way?"

No different architecture is needed. The port already speaks the real Flipper
serial protocol; the work is small tuning (connection params + pairing method),
not a rewrite. The only genuinely "easier" operational wins are: keep Wi-Fi off
while connected, and forget/re-pair when a stale bond appears.
