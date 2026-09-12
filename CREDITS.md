# Feature Origin — Merged vs Written Here

For every feature in this fork: **did it come from the merge, or was it written here?**

Nothing below is a guess. All 197 directories in the tree were tested against
[Sor3nt's ESP32 port](https://github.com/Sor3nt/Flipper-Zero-ESP32-Port) and against
[Momentum Firmware](https://github.com/Next-Flip/Momentum-Firmware) — by whether each
directory's actual **source files** appear in those trees, not by matching directory paths.

Path matching alone is wrong in both directions, so it is not used:
- Code that came with the port was sometimes **relocated** here — e.g. `mp3_player`, whose
  Helix decoder lives at `applications/main/streaming/lib/helix/` in the port but at
  `applications/main/mp3_player/lib/helix/` here. It is the port's, not mine.
- The port **kept growing** after this fork split off — `streaming`, `components/wifi`,
  `libsmb2`, `fw_ota`, `ota_updater` and more were added to it later. They are Sor3nt's too.

So every **MINE** entry below was confirmed to have *no* source file present anywhere in the
port's 5,199 files.

| Verdict | Meaning |
|---|---|
| **MERGED** | Present in Sor3nt's port. Not my work. |
| **MOMENTUM** | Absent from the port, present in Momentum Firmware. Not the port's, not mine. |
| **MINE** | Present in neither. Written in this fork. |

**Totals: 19 MINE · 3 MOMENTUM · 175 MERGED (197 directories).**

---

# Summary — the short answer

**Written here (19):** Dual Boot · Power Profiler · Wardriving · BLE Detector · Macro Pad ·
`backup_settings` · `components/multiboot` · `components/nimble_glue` ·
`components/hotspot_arcade_runtime` · `components/hotspot_arcade_service` ·
`hotspot_arcade` (integration only) · and 8 mic/audio tools (mic_common, mic_level,
mic_logger, mic_sonar, mic_waterfall, tone_gen, ultrasonic, voice_notes).

**From Momentum (3):** `momentum_app` · `lib/momentum` · `components/momentum`.

**Everything else — all 175 remaining directories — is MERGED from the port.**
That includes the whole WiFi suite (`wlan_app` — Evil Portal, Deauth, AirSnitch, Android TV,
Probe Sniffer — and `components/wifi`), plus `streaming`, `libsmb2`, `fw_ota`,
`ota_updater`, every service, every debug app, every example, all other libraries,
and 25 of the 34 community apps.

---

# 1. applications/main (26)

| App | Origin | Notes |
|---|---|---|
| archive | MERGED | OFW |
| bad_usb | MERGED | OFW |
| **ble_detector** | **MINE** | BLE scanning/profiling, unbounded scan lists |
| ble_spam | MERGED | embeds WhisperPair — [zalexdev](https://github.com/zalexdev/wpair-app), Apache-2.0 |
| clock_app | MERGED | OFW |
| doom | MERGED | doomgeneric — [ozkl](https://github.com/ozkl/doomgeneric) / id Software, GPLv2 |
| **dualboot** | **MINE** | multi-firmware boot menu, side-button recovery |
| esp_now | MERGED | **Sor3nt** — ESP32 wireless |
| gpio | MERGED | OFW |
| ibutton | MERGED | OFW |
| infrared | MERGED | OFW base; Momentum universal remotes added later |
| lfrfid | MERGED | OFW |
| **macro_pad** | **MINE** | USB/BLE HID macro recorder |
| momentum_app | **MOMENTUM** | the Momentum settings application |
| mp3_player | MERGED | came with the port; Helix decoder is RealNetworks, RPSL/RCSL |
| nfc | MERGED | OFW; ChameleonUltra support from the port |
| nrf24 | MERGED | **Sor3nt** — ESP32 wireless |
| onewire | MERGED | OFW |
| **power_profiler** | **MINE** | live power draw from the BQ27220 fuel gauge |
| streaming | MERGED | Helix decoder — RealNetworks, RPSL/RCSL |
| subghz | MERGED | OFW |
| subghz_remote | MERGED | DarkFlippers — [@gid9798](https://github.com/gid9798), [@xMasterX](https://github.com/xMasterX), MIT |
| u2f | MERGED | OFW / Momentum CTAP2 |
| **wardriving** | **MINE** | passive WiFi + BLE + Sub-GHz logger to PSRAM |
| wlan_app | MERGED | **Sor3nt** — the WiFi suite: Evil Portal, Deauth, AirSnitch, Android TV, Probe Sniffer, SMB browser |
| wolf3d | MERGED | Wolf4SDL — Moritz Kroll / id Software, GPL |

# 2. applications/settings (14)

**MERGED (13):** about · bt_settings_app · clock_settings · desktop_settings ·
dolphin_passport · expansion_settings_app · input_settings_app · interface_settings ·
notification_settings · power_settings_app · spoofing_settings · storage_settings · system

**MINE (1):** `backup_settings` — settings backup and restore.

# 3. applications/services (15) — all MERGED

bt · cli · crypto · desktop · dialogs · dolphin · expansion · gui · input · loader ·
locale · namechanger · power · rpc · storage

# 4. applications/system (6) — all MERGED

find_my_flipper · hid_app · js_app · ota_updater · snake_game · updater

# 5. applications/debug (27) — all MERGED

accessor · battery_test_app · blink_test · bt_debug_app · ccid_test · crash_test ·
direct_draw · display_test · event_loop_blink_test · expansion_test · file_browser_test ·
infrared_test · keypad_test · lfrfid_debug · loader_chaining_a · loader_chaining_b ·
locale_test · rpc_debug_app · speaker_debug · subghz_test · text_box_element_test ·
text_box_view_test · uart_echo · unit_tests · usb_mouse · usb_test · vibro_test

# 6. applications/examples (14) + drivers (1) — all MERGED

example_adc · example_apps_assets · example_apps_data · example_ble_beacon ·
example_custom_font · example_date_time_input · example_event_loop · example_images ·
example_number_input · example_plugins · example_plugins_advanced · example_thermo ·
example_view_dispatcher · example_view_holder · drivers/subghz

# 7. components (49)

**MINE (4):** `multiboot` (dynamic multi-boot layout, staged table writes, interrupted-update
recovery) · `nimble_glue` (NimBLE host integration) · `hotspot_arcade_runtime` ·
`hotspot_arcade_service` (ESP-IDF plumbing for Hotspot Arcade — the game itself is
[tarikbc](https://github.com/tarikbc/hotspot-arcade)'s, MIT)

**MOMENTUM (1):** `momentum` — settings core, asset-pack loader, PNG icon decoder

**MERGED (44):** archive · assets · bit_lib · ble_hid · ble_profile · ble_serial · bt ·
btshim · cli · compat · datetime · desktop · desktop_settings · dialogs · dolphin ·
flipper_application · flipper_format · flipper_protobuf · furi · furi_ble · furi_hal ·
fw_ota · gui · heatshrink · infrared · input · lfrfid · libsmb2 · loader · locale · mjs ·
mlib · music_worker · namechanger · nanopb · nfc · notification · power · rpc · storage ·
subghz · toolbox · u8g2 · update_util · wifi

# 8. lib (10)

**MOMENTUM (1):** `momentum`
**MERGED (9):** bit_lib · drivers · flipper_format · infrared · lfrfid · mjs ·
music_worker · subghz · toolbox

# 9. applications_user (34)

**MINE (9):** hotspot_arcade *(integration only — game is tarikbc's, MIT)* · mic_common ·
mic_level · mic_logger · mic_sonar · mic_waterfall · tone_gen · ultrasonic · voice_notes

**MERGED (25)** — authors as declared in each app's own `application.fam` / `LICENSE` / source:

| App | Author | Upstream | License |
|---|---|---|---|
| FindMyFlipper | [@MatthewKuKanich](https://github.com/MatthewKuKanich) | [FindMyFlipper](https://github.com/MatthewKuKanich/FindMyFlipper) | see upstream |
| TagTinker | i12bp8 | — | GPLv3 |
| flipper-zero_authenticator | Alexander Kopachov ([@akopachov](https://github.com/akopachov)) | [repo](https://github.com/akopachov/flipper-zero_authenticator) | GPLv3 |
| flipper_xremote | [@kala13x](https://github.com/kala13x) | [flipper-xremote](https://github.com/kala13x/flipper-xremote) | GPLv3 |
| protopirate | The Pirates' Plunder | [protopirate.net](https://protopirate.net/ProtoPirate) | GPLv3 |
| wmbuster | wm-buster / i12bp8 | [wmbuster](https://github.com/i12bp8/wmbuster) | GPLv3 |
| tamagotchi_p1 | cyanic; tamalib by Jean-Christophe Rona | [repo](https://github.com/GMMan/flipperzero-tamagotch-p1) | GPLv2 |
| blackjack, blackjack_split | Tibor Tálosi | — | MIT |
| asteroids | Salvatore Sanfilippo | — | see source |
| pong | nmrr | [github.com/nmrr](https://github.com/nmrr) | CC0-1.0 |
| dice | [@Ka3u6y6a](https://github.com/Ka3u6y6a) | [flipper-zero-dice](https://github.com/Ka3u6y6a/flipper-zero-dice) | see upstream |
| snake20 | [@Willzvul](https://github.com/Willzvul) | [Snake_2.0](https://github.com/Willzvul/Snake_2.0) | see upstream |
| mifare_fuzzer | [@spheeere98](https://github.com/spheeere98), @Sil333033 | [mifare_fuzzer](https://github.com/spheeere98/mifare_fuzzer) | see upstream |
| nRF24_jammer | W0rthlessS0ul | [FZ_nRF24_jammer](https://github.com/W0rthlessS0ul/FZ_nRF24_jammer) | see upstream |
| nfc_rfid_detector | SkorP | — | see upstream |
| reverse_shell, rfid2_reader | cardputer | — | see upstream |
| roulette | Superagent | — | see upstream |
| game15, nfc_magic, t_embed_snake, tetris, texas_holdem, weather_station | not declared in-tree | — | unknown |

---

# Foundation

| Project | Author | Role |
|---|---|---|
| [Flipper-Zero-ESP32-Port](https://github.com/Sor3nt/Flipper-Zero-ESP32-Port) | **Sor3nt** | the port this fork is built on — HAL, board bring-up, web flasher, wireless tooling, OTA and SD update |
| [Flipper Zero firmware](https://github.com/flipperdevices/flipperzero-firmware) | Flipper Devices Inc. | Furi OS, GUI, services, app framework (GPLv3) |
| [Momentum Firmware](https://github.com/Next-Flip/Momentum-Firmware) | The Momentum team | dolphin, passport, settings, menu styles, asset packs (GPLv3) — see [NOTICE_MOMENTUM_PORT.md](NOTICE_MOMENTUM_PORT.md) |
| [ESP-IDF](https://github.com/espressif/esp-idf) | Espressif Systems | SDK and toolchain (Apache-2.0) |

# Caveats

- **`macro_pad`** — written here (confirmed by the author). Absent from the port and from
  Momentum; none of its source files appear anywhere in the port's tree.
- **`wardriving`** — written here, on top of Sor3nt's WiFi stack. The WiFi *application*
  (`wlan_app`) and driver (`components/wifi`) are the port's; this logger app is not.
- **`hotspot_arcade`** is marked MINE for the *integration only*; the game itself is
  [tarikbc](https://github.com/tarikbc/hotspot-arcade)'s (MIT).
- Several MERGED community apps ship no `LICENSE`; their terms come from their upstreams.
- No Momentum asset-pack artwork is redistributed here.
