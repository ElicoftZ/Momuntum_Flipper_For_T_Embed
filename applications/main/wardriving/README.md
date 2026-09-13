# Wardriving

Wi-Fi Wardriving is temporarily hidden from the menu. Bluetooth and Sub-GHz
remain available; the Wi-Fi scanner code is retained for later use.

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
