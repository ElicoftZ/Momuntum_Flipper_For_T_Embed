# Wardriving

Sub-GHz and BLE wardriving are available from the menu. Wi-Fi wardriving is
hidden from the menu (the mode/worker code stays in the tree, just
unreachable): it's the one mode that turns Wi-Fi on, and this board has a
real, unresolved Wi-Fi-vs-Bluetooth memory conflict where BLE controller init
fails while Wi-Fi is running. Everything below (detection, emulation) is
pure BLE and unaffected.

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

BLE scan interruptions and startup errors remain visible instead of being
overwritten.

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

This mode takes over Bluetooth the same way BLE Wardriving/BLE Detector do,
so it hits the same Wi-Fi-vs-Bluetooth limitation noted above: turn the
board's Wi-Fi off first if it fails to start.

## Location

There's no GPS and Wi-Fi wardriving (the only on-device location source) is
hidden — location isn't computed on the device. Join `findmy_<date>.csv`
(timestamped tag detections) against whatever location data you collect
separately on a PC by timestamp instead. `tools/build_apdb.py` and the
`WRAPDB1` AP-database format stay in the tree unused, in case Wi-Fi
wardriving is ever re-enabled later.

