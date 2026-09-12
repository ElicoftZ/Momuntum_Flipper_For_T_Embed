> [!CAUTION]
> ## 🚨 SECURITY WARNING — DO NOT FLASH "L15Dev" FIRMWARE 🚨
>
> A firmware build distributed under the name **"L15Dev" / "Bitwire"** has been reported to contain **malware (a virus) and a backdoor**. **Do not download, flash, or run it under any circumstances.**
>
> Only use the official builds from this repository / the [web flasher](https://elicoftz.github.io/Momuntum_Flipper_For_T_Embed/interface.html). If you already flashed an "L15Dev" image, re-flash a clean official build and treat any credentials/data on the device (WiFi passwords, captures) as compromised.

> WARNING: I do not take responsibility if you damage your board or property. This project is for educational purposes only — proceed at your own risk.

# Momentum T-Embed — Flipper Zero ESP32 Port

A port of the [Flipper Zero](https://flipperzero.one/) firmware — with the **Momentum** feature set — to the **LilyGo T-Embed CC1101** and other ESP32 boards. It brings the Flipper Zero UI, services, and application framework to affordable ESP32 hardware — no Flipper Zero required.

## ✨ New in v2

- **Dual Boot** — install and switch between multiple firmwares from a boot menu, with a hardware side-button recovery escape hatch.
- **OTA firmware updates** — update over WiFi from *Settings → Update Firmware*; keeps SD-card files in sync. No cable, no toolchain. [ Thank to Sor3nt for adding feature in his original port ]
- **Control Centre** — the lock menu is now a full quick-settings panel (Bluetooth, WiFi, Dark mode, Wake, PC Link, USB Storage, Web-Filesystem, Mesh, Dual Boot) with **brightness & volume sliders**.
- **Wardriving** — passive WiFi + BLE + Sub-GHz logger.
- **New WiFi tools** — AirSnitch, Probe Sniffer, Smart Deauth, SMB Browser, Web-Filesystem, Android TV Remote.
- **BLE Detector** & **Macro Pad** (USB/BLE HID recorder).
- **Streaming** — unified music & video (AirPlay, Chromecast, DLNA). [ Thank to Sor3nt for adding feature in his original port ]
- **U2F / FIDO2 (CTAP2)** security key — PIN & passkeys.
- **New apps & games** — Hotspot Arcade, NFC Magic, MIFARE Fuzzer, NFC/RFID Detector, RFID2 Reader, Reverse Shell, Roulette, WMBuster, TagTinker. [ Thank to Sor3nt for adding apps in his original port ]
- **Interface** & **Spoofing** settings — main-menu customization; device name / shell color.

See [RELEASE_NOTES_v2.md](RELEASE_NOTES_v2.md) for the full v2 changelog.

## Supported Boards

| Board | MCU | Display | Input | SubGHz | NFC | IR | SD Card |
|---|---|---|---|---|---|---|---|
| **LilyGo T-Embed CC1101** | ESP32-S3 (Xtensa LX7) | ST7789 320×170 | Rotary encoder + button | CC1101 | PN532 (I2C) | RMT TX + RX | SPI |
| **Waveshare ESP32-C6-LCD-1.9** | ESP32-C6 (RISC-V) | ST7789V2 320×172 | CST816S touch | — | — | — | SPI |
| **Waveshare ESP32-C6-LCD-1.47** ⚠️ | ESP32-C6 (RISC-V) | JD9853 320×172 | AXS5106L touch | — | — | — | SPI |
| **DIY ESP32-S3 with 2.8" TFT** ⚠️ | ESP32-S3 (Xtensa LX7) | 2.8" ILI9341 320×240 | 6× Tactile buttons | CC1101 | PN532 (I2C) | TX | SPI |

> ⚠️ **Waveshare ESP32-C6 boards — limited.** The ESP32-C6 has only **512 KB SRAM and no PSRAM**, so RAM-heavy apps are effectively non-functional. In particular WiFi monitor mode / handshake capture fails (`esf_buf_setup_static: alloc eb fail` → `ESP_ERR_NO_MEM`). Treat these boards as usable only for lightweight apps. The full feature set targets the PSRAM-equipped **T-Embed**.

## How to Flash

The easiest way is the **web flasher** — no toolchain required, just a Chrome/Edge browser and a USB cable:

**[▶ Flash via Browser](https://elicoftz.github.io/Momuntum_Flipper_For_T_Embed/interface.html)**

Connect your board, click flash, done. After flashing, copy the contents of **[sdcard.zip](https://github.com/ElicoftZ/Momuntum_Flipper_For_T_Embed/releases/latest/download/sdcard.zip)** onto a FAT32 SD card and insert it — most apps need files there to function.

## Firmware Update

The T-Embed can update itself — no PC toolchain or re-flashing required:

- **Over WiFi (OTA):** *Settings → Update Firmware* checks the release server for a newer build, downloads and installs it in the background, then reboots. It also keeps your SD card files in sync.
- **Dual Boot:** install and switch between multiple firmware images from the boot menu; hold the side/Back button at power-on for ~3 s for a hardware recovery back to this firmware.

> OTA requires the dual-OTA partition layout. A device on the old single-app layout has to be flashed once via the web flasher before wireless OTA updates work.

## Apps

### 📡 Wireless / RF

**Sub-GHz** (external CC1101, 433–868 MHz) — receive & decode, Read RAW to `.sub`, frequency analyzer, HackRF-style RF spectrum analyzer, band hopper, transmit/manual signal creation, brute-force with manufacturer dictionaries, playlists, and TPMS decoding (Schrader, Citroën, Ford, Renault, Toyota, generic). *AES-encrypted manufacturer keystores are not decryptable on this port.*

**WiFi** — full pentest toolkit:
- Scanner, Connect (WPA/WPA2/WPA3 + saved passwords), Deauther & **Smart Deauth**, Sniffer (PCAP), Handshake capture (EAPOL), **AirSnitch**, **Probe Sniffer**, Beacon Spam
- Network Scan / Port Scan, Web Crawler
- **Evil Portal** — captive portal with credential harvesting, custom templates, and an optional internet bridge (STA uplink + NAPT + DNS)
- **Web-Filesystem** — HTTP SD-card server (drag & drop), **SMB Browser** (Windows/macOS/NAS), **Android TV Remote**
- **WiFi toggle** — persistent global on/off from the Control Centre, auto-reconnect
- **Wardriving** — passive WiFi + BLE + Sub-GHz logger

**Mesh / Buddy (ESP-NOW)** — pair headless ESP32 "buddy" boards to offload WiFi handshake capture with durable store-and-forward.

**Bluetooth** — BLE Spam (Apple/Google/Microsoft/Samsung/Xiaomi), BLE Walk (GATT scanner), **BLE Detector**, WhisperPair, BLE Clone, FindMy (AirTag/SmartTag/Tile), BLE HID.

**NRF24** (external nRF24L01) — 2.4 GHz spectrum analyzer, jammer, MouseJacker.

**Infrared** (RMT TX/RX) — learn/replay, universal remotes (TV/AC/audio/projectors/fans/LEDs), brute force.

### 🪪 NFC (PN532 over I2C)
Read/save/emulate/write cards, dictionary attacks, 14 protocols, 30+ card auto-parsers (Charlie Card, Clipper, EMV, Gallagher, HID, Opal, Troika, …), plus **NFC Magic**, **MIFARE Fuzzer**, **NFC/RFID Detector**, and the **Passport (MRTD)** reader.

### 🔑 Security — U2F / FIDO2
USB security-key support: **U2F/FIDO1** and full **FIDO2/CTAP2** (PIN, resident keys/passkeys, self-attestation via mbedtls). Cert/key in `/ext/u2f/assets/`. Requires USB-OTG (T-Embed).

### ⌨️ HID / USB
**Bad USB** (Ducky-script over USB or BLE, ~30 layouts) and **Macro Pad** (USB/BLE HID macro record & playback).

### 🎵 Media
**Streaming** — unified music (.mp3) & video (.mp4) player: play locally or stream to AirPlay, Chromecast/Google Cast, and DLNA devices.

### 🎮 Games
Doom (needs `doom1.wad`), Snake, Hotspot Arcade, and 30+ user FAPs (Asteroids, Blackjack, Pong, Roulette, Tamagotchi, Tetris, Texas Hold'em, …).

### 🛠 System / Tools
**Control Centre** (lock-menu quick settings + sliders), **Dual Boot**, **Archive** (SD browser), **JS Runner** (mJS), qFlipper bridge & USB Storage (USB-OTG), and a 30-level Momentum **dolphin** with animated idle desktop.

### ⚙ Settings
Bluetooth, backlight, clock, dolphin/passport, expansion port, input, notification, power, storage, system info, factory reset, **Update Firmware** (OTA), **Interface** (main-menu customization), and **Spoofing** (device name / shell color).

## SD Card Layout

| Path | Used by |
|---|---|
| `/ext/Manifest` | Desktop (presence check) |
| `/ext/dolphin/` + `manifest.txt` | Idle animations |
| `/ext/apps_assets/nfc/plugins/` | NFC protocol plugins (.fal) |
| `/ext/apps_data/doom/doom1.wad` | Doom |
| `/ext/badusb/` | Bad USB scripts + `assets/layouts/*.kl` |
| `/ext/infrared/assets/` | Universal remote DBs |
| `/ext/nfc/assets/` | MIFARE & EMV dictionaries |
| `/ext/subghz/assets/` | SubGHz keystores |
| `/ext/u2f/assets/` | U2F cert + key |
| `/ext/wifi/<ssid>.txt` | Saved WiFi passwords |
| `/ext/wifi/evil_portal/` | Custom captive-portal templates |

A complete starter kit is in [sdcard.zip](https://github.com/ElicoftZ/Momuntum_Flipper_For_T_Embed/releases/latest/download/sdcard.zip) — extract it onto a FAT32 SD.

## Building

Requires **[ESP-IDF v5.4.1](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32s3/get-started/)** (exact version).

### Linux / macOS
```bash
./buildAndFlash_T-Embed.sh            # T-Embed (build + flash)
./buildAndFlash_T-Embed.sh --build-only
```

### Windows
```bat
:: source ESP-IDF v5.4.1 first (C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat)
idf.py -B build_multiboot -DFLIPPER_BOARD=lilygo_t_embed_cc1101 -DSDKCONFIG=build_multiboot/sdkconfig build
```
The dual-boot shipping image is built in `build_multiboot/`. Flash the app at offset `0x20000`.

### Build a FAP
```bash
./buildFap.sh applications/main/my_app   # firmware must be built first
```

## Porting Approach

Preserves the original Flipper Zero architecture as closely as possible:
- **Furi OS** on FreeRTOS with the same thread/mutex/event/record API
- **HAL** maps STM32 peripherals to ESP-IDF (SPI → `esp_lcd`, I2C → PN532/CST816S, RMT → IR, NimBLE → BLE, TinyUSB → USB-HID)
- **Display** renders the original 128×64 mono framebuffer, then 2× upscales to RGB565
- **Crypto** uses real mbedtls (no Flipper-Enclave key — affects encrypted SubGHz keystores only)

## Credits & Acknowledgements

This project stands entirely on the work of others. It is a **fork** built on top of, and grateful to:

- **[Flipper Zero firmware](https://github.com/flipperdevices/flipperzero-firmware)** — Flipper Devices Inc. The original firmware, UI, Furi OS, services and application framework that everything here derives from. *(GPLv3)*
- **[Momentum Firmware](https://github.com/Next-Flip/Momentum-Firmware)** — the Momentum team. The custom-firmware feature set (dolphin, passport, settings, apps) this port brings over. *(GPLv3)* — see [NOTICE_MOMENTUM_PORT.md](NOTICE_MOMENTUM_PORT.md).
- **[Flipper-Zero-ESP32-Port](https://github.com/Sor3nt/Flipper-Zero-ESP32-Port)** — **Sor3nt**. The ESP32 / ESP-IDF port this repository is a fork of; the HAL, board bring-up, web flasher and much of the wireless tooling originate there.
- **[ESP-IDF](https://github.com/espressif/esp-idf)** — Espressif Systems. The SDK and toolchain this port targets.

T-Embed fork maintained by **[ElicoftZ](https://github.com/ElicoftZ)**.

### Bundled third-party components

Each retains its own license — see the `LICENSE` / `COPYING` / `NOTICE` file next to it in the tree:

| Component | Used for | Upstream |
|---|---|---|
| doomgeneric | Doom | [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric) |
| Helix | MP3 decoding (Streaming) | RealNetworks Helix |
| libsmb2 | SMB2/3 client | [sahlberg/libsmb2](https://github.com/sahlberg/libsmb2) *(LGPL-2.1)* |
| heatshrink | compression | [atomicobject/heatshrink](https://github.com/atomicobject/heatshrink) |
| mJS | JS Runner | [cesanta/mjs](https://github.com/cesanta/mjs) |
| Hotspot Arcade | arcade app | [tarikbc/hotspot-arcade](https://github.com/tarikbc/hotspot-arcade) |
| WPair | WhisperPair | [zalexdev/wpair-app](https://github.com/zalexdev/wpair-app) — see [NOTICE](applications/main/ble_spam/whisper_pair/NOTICE) |

Additional community apps under `applications_user/` — TagTinker, Flipper Authenticator, xRemote, Tamagotchi (tamalib), Blackjack, ProtoPirate, WMBuster, Wolf3D (Wolf4SDL) and others — each ship under their own `LICENSE`.

> No Momentum asset-pack artwork is redistributed here. If you add asset packs, audit and preserve each asset's own license and attribution before distributing.

## License

Licensed under the **[GNU General Public License v3.0](LICENSE)**, inherited from Flipper Zero and Momentum. Bundled components are covered by their own licenses listed above.

**If you distribute binaries of this firmware, you must comply with GPLv3** — make the complete corresponding source available and preserve the license texts and attribution of every bundled component. This README is not a substitute for the full license texts or legal advice.
