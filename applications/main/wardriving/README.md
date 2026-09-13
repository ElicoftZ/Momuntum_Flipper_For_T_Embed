# Wardriving

Sub-GHz, BLE, and Wi-Fi wardriving are all available from the menu.

Wi-Fi and BLE results show the vendor/brand first, followed by the full MAC
address, the advertised name/SSID, and signal/channel details. Up/Down selects
another result. Random/local addresses remain visible and are logged.

Copy `sdcard/apps_data/wifi/mac-vendor.txt` to `/apps_data/wifi/mac-vendor.txt`
on the SD card. The table contains IEEE MA-L vendor assignments downloaded from
https://standards-oui.ieee.org/oui/oui.csv. Without the table, MAC addresses still
work and the scanner reports `OUI DB missing` for public addresses.

Public addresses use the OUI table. Locally administered Wi-Fi addresses and
random BLE addresses do not use OUI guesses. Apple/Samsung manufacturer data
in BLE advertisements supplies a company hint even for random addresses.
These hints identify an advertised company, not a verified phone brand/model.
Wi-Fi scanning discovers access points/hotspots, not all nearby client phones.
CSV `vendor` values retain `OUI:` or `Adv:` to distinguish the source.

Wi-Fi wardriving disconnects background station association before scanning,
then returns Wi-Fi to its normal configured state when stopped. Failed scans
are shown and retried; an empty successful scan is not a failure. BLE scan
interruptions and startup errors remain visible instead of being overwritten.

Run `python tests/host/wardriving_regression.py` in an MSVC developer shell for
mocked radio error/cleanup and advertisement parsing checks. Build the T-Embed
NimBLE configuration for compilation/link validation. On hardware, check Wi-Fi
with an unavailable saved network, BLE public/random advertisements, missing
OUI database, and Back/reopen in each scanner.

## FindMy tracker settings

BLE wardriving always tags AirTag, Google, and Samsung tracker hits as they are
detected — Apple FindMy mfg `0x004C` type `0x12`; Samsung mfg `0x0075` or
service `0xFD5A`; Google FindMy-Device company `0x00E0` or Fast Pair service
`0xFE2C`. Every BLE device is still logged; tagged hits show the brand in the
scrollable list.

The main menu's **FindMy Settings** screen (a scrollable `variable_item_list`)
has a single choice:

- **Save map on detect** — Yes/No. When Yes, each tagged hit is appended to
  `/wardriving/findmy_<date>.csv` (timestamp, brand, name, id, rssi,
  best_rssi, channel) in addition to the normal wardriving log.

The choice is stored in `/apps_data/wardriving/findmy.txt`.

There is no GPS in this port, so the FindMy CSV records the RTC timestamp of
each detection, not coordinates. Real coordinates come from the FindMy cloud
(Apple/Google/Samsung) after the tag is located by nearby phones; that lookup
runs on a PC/phone, not on the device.

## FindMy tag emulation

The main menu's **FindMy Emulate** entry broadcasts the board itself as a
lost-item tracker, using the tag type chosen in **FindMy Settings ->
Emulate Tag** (AirTag / Google / Samsung, stored alongside the detection
setting in the same `findmy.txt`):

- **AirTag** — the same static Apple FindMy advertisement (mfg `0x004C`, type
  `0x12`) that `find_my_flipper` broadcasts by default.
- **Samsung** — a Galaxy Buds "Easy Setup" frame (mfg `0x0075`), reused from
  `ble_spam`'s payload builders.
- **Google is not implemented.** Google's Find My Device Network beacon is a
  rotating identifier cryptographically derived from a key provisioned
  through a real Google account; there is no way to produce a valid-looking
  one without that key, unlike Apple/Samsung's effectively static payloads.
  Selecting Google shows "Google emulation unsupported" and broadcasts
  nothing, rather than emitting a frame that would just be wrong.

Emulation and BLE scanning cannot run at the same time — they share the
radio — so switching modes always fully stops one before starting the other.
Press Back to stop broadcasting and return to the menu.

## Offline location (no internet)

Wi-Fi wardriving can estimate its own position **without any internet** using a
preloaded AP-location database on the SD card:

1. Build `apdb.bin` on a PC from a CSV that maps BSSIDs to coordinates (a WiGLE
   wardriving export or network dump works directly):
   `python tools/build_apdb.py input.csv apdb.bin`
2. Copy it to `/ext/apps_data/wardriving/apdb.bin`.
3. Run **WiFi Wardriving**. Each scan looks the seen BSSIDs up locally and
   estimates a position as an RSSI-weighted centroid of the matches. No network
   access is used.

The estimate is written to `/wardriving/location_<date>.csv`:

```
timestamp,latitude,longitude,matched_aps
```

`matched_aps` is how many scanned BSSIDs were found in the database; a higher
number means a more trustworthy fix. The scan screen shows the current estimate
in the bottom-right corner. Because the position comes from the database, not a
satellite, coverage and accuracy depend entirely on the AP data you preload —
dense urban areas are good, rural areas may produce no fix. The file format is
little-endian: `WRAPDB1\0` + `uint32` count + sorted records of `BSSID[6]`,
`int32 lat*1e7`, `int32 lon*1e7`.


