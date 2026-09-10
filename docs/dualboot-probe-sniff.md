# Dual Boot repair and Probe Sniff

This update is for the LilyGO T-Embed CC1101 running this project's 16 MB
multiboot layout, with Momentum's factory app at 0x20000.

## Apply the update

Both `bootloader.bin` and `momentum.bin` must be updated. An app-only SD update
cannot repair the old bootloader. The previous build contained the empty
bootloader hook, so a verified install could remain in the pending partition
table and never appear in the installed firmware menu.

From an ESP-IDF terminal in this package folder, replace COM14 with the device's
port and run:

```
python -m esptool --chip esp32s3 --port COM14 write_flash 0x0 bootloader.bin 0x20000 momentum.bin
```

These two writes preserve the partition table, pending install, NVS, secondary
firmware slots, and SD files. Do not erase flash or write a fresh merged image
when preserving installed firmware. This update command is only for the
multiboot layout described above.

After restarting, open Dual Boot. Recognized firmware appears as `Boot <name>`;
select it, then `Boot firmware`. Matching SD copies remain on the card but are
excluded from install choices. A valid pending install from the old bootloader
is applied during reboot. New installations also require a restart to apply
their partition layout.

## WiFi > Probe Sniff

- OK or Right: start/stop passive probe-request capture.
- Up/Down: browse device/network pairs.
- Left: switch between automatic channel hopping and holding the current channel.
- Back: stop recording, flush the file, and return to WiFi.

The live display shows MAC address, requested SSID or wildcard, RSSI, channel,
probe count, and last-seen age. Long SSIDs wrap; non-printable SSID bytes display
as dots, with original bytes retained in PCAP. The live list holds 64 distinct
MAC/SSID pairs and reports when full; recording continues beyond that limit.

PCAP files are saved under `/ext/wifi/probes/` with unique timestamped names.
The recorder retains frames up to 4096 bytes, including probe information
elements, and shows recording drops. Capturing without a writable SD card
still provides the live view and reports `SD unavailable: live only`.
Starting capture disconnects the current WiFi connection.

WiFi startup now retries with the BLE host/controller suspended if its first
attempt fails. This frees internal RAM without changing the saved Bluetooth
preference. BLE is restored after a temporary WiFi app closes, or when persistent
WiFi is turned off. If WiFi still cannot start, the failed driver is cleaned up
and BLE is restored. A Bluetooth switch explicitly turned off stays off.

This board captures 2.4 GHz channels allowed by its configured country, one
channel at a time. It cannot capture 5/6 GHz or guarantee every transmitted
probe while hopping. Randomized MAC addresses remain as observed.

## Validation

Host tests cover probe parsing, malformed/truncated information elements,
wildcard SSIDs, dynamic firmware layout allocation, and recovery from
interrupted partition writes. The package script checks that the recovery
handler and Probe Sniff scene are linked into the built firmware.
Live radio reception, display layout, and boot switching still need device testing.
