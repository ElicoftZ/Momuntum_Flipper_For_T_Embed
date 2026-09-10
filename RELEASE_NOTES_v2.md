# Momentum T-Embed — v2

Firmware for the **LilyGo T-Embed CC1101** (ESP32-S3). Flash it in your browser:
**[elicoftz.github.io/Momuntum_Flipper_For_T_Embed](https://elicoftz.github.io/Momuntum_Flipper_For_T_Embed/interface.html)**

> After flashing, extract [`sdcard.zip`](https://github.com/ElicoftZ/Momuntum_Flipper_For_T_Embed/releases/latest/download/sdcard.zip) onto a FAT32 microSD and insert it — most apps need files there.

---

## ✨ New in v2

**System & updates**
- **Dual Boot** — install and switch between multiple firmwares from a boot menu, with a hardware side-button recovery escape hatch back to this firmware.
- **OTA firmware updates** — *Settings → Update Firmware* downloads and installs a newer build over WiFi and keeps your SD-card files in sync. No cable, no toolchain.
- **Control Centre** — the lock menu is now a full quick-settings panel: Bluetooth, WiFi, Dark mode, Wake, PC Link, USB Storage, Web-Filesystem, Mesh, Dual Boot, plus **brightness & volume sliders**.
- **Interface** and **Spoofing** settings — customize the main menu layout, and change the device name / shell color.

**WiFi**
- **Wardriving** — passive WiFi + BLE + Sub-GHz logger (PSRAM).
- **AirSnitch** — probe-based host/network discovery.
- **Probe Sniffer** — passive 802.11 probe-request capture to PCAP.
- **Smart Deauth** — station-aware targeted deauthentication.
- **SMB Browser** — browse and download from Windows / macOS / NAS shares to the SD card.
- **Web-Filesystem** — manage the SD card over HTTP from any browser (drag & drop).
- **Android TV Remote** — pair and control Android / Google TV / Chromecast.

**Apps & hardware**
- **BLE Detector** — rapid Bluetooth device scanning & profiling.
- **Macro Pad** — USB/BLE HID macro recorder & playback.
- **Streaming** — unified music & video player (AirPlay, Chromecast, DLNA).
- **U2F / FIDO2 (CTAP2)** security key — passwordless login with PIN & passkeys (USB-OTG).
- **New user apps & games** — Hotspot Arcade, NFC Magic, MIFARE Fuzzer, NFC/RFID Detector, RFID2 Reader, Reverse Shell, Roulette, WMBuster, TagTinker.

**Under the hood**
- Dual-OTA partition layout, recovery bootloader, and DRAM-safe WiFi bring-up so WiFi and BLE coexist.
- Persistent device name, and dolphin XP / level progress that survive sleep and reboot.

---

## Install

1. Open the **[web flasher](https://elicoftz.github.io/Momuntum_Flipper_For_T_Embed/interface.html)** in Chrome or Edge, connect your board over USB, and click flash.
2. Download **[`sdcard.zip`](https://github.com/ElicoftZ/Momuntum_Flipper_For_T_Embed/releases/latest/download/sdcard.zip)**, extract its contents onto a FAT32 microSD, and insert it.
3. On a device already on the dual-OTA layout, you can instead update wirelessly via *Settings → Update Firmware*.

## Notes
- Full feature list and build instructions: see the [README](README.md).
- ⚠️ Only flash official builds from this repository / the linked web flasher.
