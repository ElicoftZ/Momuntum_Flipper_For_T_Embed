# Momentum T-Embed — v2

Firmware for the **LilyGo T-Embed CC1101** (ESP32-S3). Flash it in your browser:
**[elicoftz.github.io/Momuntum_Flipper_For_T_Embed](https://elicoftz.github.io/Momuntum_Flipper_For_T_Embed/interface.html)**

> After flashing, extract [`sdcard.zip`](https://github.com/ElicoftZ/Momuntum_Flipper_For_T_Embed/releases/latest/download/sdcard.zip) onto a FAT32 microSD and insert it — most apps need files there.

---

## ✨ New in v2

**System & updates**
- **Dual Boot** — install and switch between multiple firmwares from a boot menu, with a hardware side-button recovery escape hatch back to this firmware.
- **OTA firmware updates** — *Settings → Update Firmware* downloads and installs a newer build over WiFi and keeps your SD-card files in sync. No cable, no toolchain. [ Thank to Sor3nt for adding feature in his original port ]
- **Control Centre** — the lock menu is now a full quick-settings panel: Bluetooth, WiFi, Dark mode, Wake, PC Link, USB Storage, Web-Filesystem, Mesh, Dual Boot, plus **brightness & volume sliders**.
- **Interface** and **Spoofing** settings — customize the main menu layout, and change the device name / shell color.

**WiFi**
- **Wardriving** — passive WiFi + BLE + Sub-GHz logger (PSRAM).
- **AirSnitch** — probe-based host/network discovery.
- **Probe Sniffer** — passive 802.11 probe-request capture to PCAP.
- **Smart Deauth** — station-aware targeted deauthentication.
- **SMB Browser** — browse and download from Windows / macOS / NAS shares to the SD card. [ Thank to Sor3nt for adding feature in his original port ]
- **Web-Filesystem** — manage the SD card over HTTP from any browser (drag & drop). [ Thank to Sor3nt for adding feature in his original port ]
- **Android TV Remote** — pair and control Android / Google TV / Chromecast. 

**Apps & hardware**
- **BLE Detector** — rapid Bluetooth device scanning & profiling.
- **Macro Pad** — USB/BLE HID macro recorder & playback.
- **Streaming** — unified music & video player (AirPlay, Chromecast, DLNA). [ Thank to Sor3nt for adding feature in his original port ]
- **U2F / FIDO2 (CTAP2)** security key — passwordless login with PIN & passkeys (USB-OTG).
- **New user apps & games** — Hotspot Arcade, NFC Magic, MIFARE Fuzzer, NFC/RFID Detector, RFID2 Reader, Reverse Shell, Roulette, WMBuster, TagTinker. [ Thank to Sor3nt for adding feature in his original port ]
**Under the hood**
- Dual-OTA partition layout, recovery bootloader, and DRAM-safe WiFi bring-up so WiFi and BLE coexist.
- Persistent device name, and dolphin XP / level progress that survive sleep and reboot.

---

## Install

### Option A — Web flasher (easiest, no tools)
Open the **[web flasher](https://elicoftz.github.io/Momuntum_Flipper_For_T_Embed/interface.html)** in Chrome or Edge, connect your board over USB, pick **LilyGo T-Embed CC1101**, and click flash.

### Option B — One merged file (recommended for a single-command flash)
Download **`momentum_t_embed_cc1101_v2_merged.bin`** below — bootloader + partition table + app in one image — and flash it at `0x0`:

```
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash 0x0 momentum_t_embed_cc1101_v2_merged.bin
```

Add `-p COM4` (Windows) or `-p /dev/ttyACM0` (Linux/macOS) if the port isn't auto-detected. This is the easiest CLI option — one file, one offset.

### Option C — Three separate .bin files
Prefer the individual images? Download the three assets below and flash each at its offset with [esptool](https://github.com/espressif/esptool) (`pip install esptool`):

| File | Flash offset |
|---|---|
| `bootloader.bin` | `0x0` |
| `partition-table.bin` | `0x8000` |
| `furi_esp32.bin` | `0x20000` |

Run this from the folder where you downloaded them (one command, all three):

```
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x20000 furi_esp32.bin
```

- Add your port if it isn't auto-detected: `-p COM4` (Windows) or `-p /dev/ttyACM0` (Linux/macOS).
- Not erasing the OTA-data region at `0x10000` is intentional — it boots the factory image.
- **Updating an existing install?** You can flash just `furi_esp32.bin` at `0x20000` and skip the bootloader/partition-table.

### Then — set up the SD card
Download **[`sdcard.zip`](https://github.com/ElicoftZ/Momuntum_Flipper_For_T_Embed/releases/latest/download/sdcard.zip)**, extract **its contents** onto the root of a **FAT32** microSD, and insert it — most apps need files there.

## Notes
- Full feature list and build instructions: see the [README](README.md).
- ⚠️ Only flash official builds from this repository / the linked web flasher.
