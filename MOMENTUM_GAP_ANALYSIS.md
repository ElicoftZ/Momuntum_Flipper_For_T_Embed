# Momentum vs. T-Embed port — feature gap analysis

Compared `Momentum-Firmware/` against `Flipper-Zero-ESP32-Port/` on 2026-08-24.

Method matters here: **source presence is not the same as being built.** The port uses an
explicit allowlist in `fam_config.py` (`APPS = [...]`), and the authoritative built set is
`build_t_embed/esp-idf/main/generated/applications.c`. Several apps exist in the port's
source tree and are never compiled. Everything below was checked against the *built* list.

---

## 1. Apps in Momentum, source present in the port, NOT built

These are missing purely because they are absent from the `APPS` allowlist in `fam_config.py`.
Effort is unknown until each is tried — the reason they were left out is not recorded, and for
some it is likely a hardware or input-model problem rather than an oversight.

| App | Momentum path | Note |
|---|---|---|
| **iButton** | `applications/main/ibutton` | Needs a 1-Wire pin on the T-Embed; check `BOARD_PIN_*` |
| **GPIO** | `applications/main/gpio` | Pin header differs from the Flipper's; UI is also Left/Right heavy |
| **OneWire** | `applications/main/onewire` | Same 1-Wire hardware question as iButton |
| **HID remote** | `applications/system/hid_app` | BLE/USB remote. `ble_hid` component exists, so plausible |
| **Desktop settings** | `applications/settings/desktop_settings` | Favourites, PIN lock, auto-lock. `components/desktop_settings` exists |
| **Input settings** | `applications/settings/input_settings_app` | |
| **Expansion settings** | `applications/settings/expansion_settings_app` | Expansion module protocol — may not apply to this board |
| **Clock settings** | `applications/settings/clock_settings` | The port builds `clock` (the app), not the settings |
| **System settings** | `applications/settings/system` | |
| **Updater** | `applications/system/updater` | OTA self-update; probably deliberate — the port flashes over USB |

**Not folded in elsewhere.** I checked `momentum_app.c` (90 KB, a flat rewrite of Momentum's
scene tree for the Up/Down/OK/Back input model) — it only carries lockscreen-related entries,
so Desktop/Input/Expansion/Clock/System settings are genuinely unreachable, not relocated.

## 2. SubGHz protocol decoders — CORRECTED, and DONE

The raw file count (94 vs 68) overstated this badly. What actually matters is the **registry**,
`lib/subghz/protocols/protocol_items.c` — a decoder that is compiled but unregistered never
fires. Comparing registry *entries* rather than counting files:

**Only 3 protocols were genuinely missing**: `pocsag` (pager), `hormann_bisecur` (garage
doors), `x10` (home automation). All three are now copied in, adapted and registered, and the
port's registry is byte-for-byte the same protocol set as Momentum's — plus 4 TPMS protocols
(`citroen`, `ford`, `pmv107j`, `renault`) that Momentum does not have.

The other ~27 files are the **weather-station decoders**, and Momentum does *not* register
them centrally either. They are compiled into `lib/subghz` for the benefit of the external
Weather Station and Pager **FAP apps**, which build their own registry. The port ships no
external FAPs at all (`FLIPPER_EXTERNAL_APPS[]` is empty), so copying those decoders in would
add dead code, not features. **Weather-station support is an external-app question, not a
protocol question** — reopen it only if external FAPs are ever built.

Three things had to be adapted, all because the port's SubGHz core is an older fork:

- `SubGhzProtocolDecoder` here has no `get_hash_data_long` or `get_string_brief`. The
  hash functions were changed to return `uint8_t`, XOR-folding the 32-bit value rather than
  truncating it, and the brief-string hooks dropped.
- `pocsag` needs `pcsg_generic`, which needs `blocks/generic_i.h` and
  `subghz_block_generic_serialize_common()` — neither existed here. `generic.c` was split the
  same way upstream Momentum splits it, so `serialize` now calls `serialize_common`. Existing
  behaviour is unchanged; the file header written is identical.
- `pocsag.c` included `<furi/core/string.h>`, which is not on this port's include path.

## 3. Momentum settings fields not in the port

From `lib/momentum/settings.h` vs the port's `lib/momentum/settings_core.h`:

**Not applicable to this hardware** (no action needed): `rgb_backlight` (a Flipper RGB mod),
`spi_cc1101_handle` / `spi_nrf24_handle` (external GPIO modules — the T-Embed's CC1101 is
built in), `uart_esp_channel` / `uart_nmea_channel` (WiFi devboard UART routing).

**Genuine gaps, small:** `battery_icon` (battery icon styles), `favorite_timeout`,
`rpc_color_bg` / `rpc_color_fg` (screen-streaming colours), `spoof_color`.

**Deliberately absent:** `bad_pins_format` — reverted on purpose, must stay that way.

## 4. Services

`applications/services/region` has no counterpart. The port has `furi_hal_region.c` but not
the region *service* that provisions SubGHz region limits. Low impact for a personal build;
worth knowing before touching SubGHz TX limits.

## 5. Confirmed at parity — do not spend time here

- **NFC protocols** — directory sets identical
- **NFC supported-card parsers** — 40 vs 40
- **JS modules** — identical except `js_serial`, `js_gpio`, `js_i2c`, `js_spi`, which
  `fam_config.py` documents as excluded pending HAL porting
- **Infrared universal assets** — identical (9 files)
- **Debug apps** — identical source sets
- **Dolphin animations** — 35 vs 36; the port ships them on the SD card (`sdcard/dolphin/`)
  rather than in `assets/dolphin/external/`. Same feature, different delivery.
- **BadUSB layouts** — port has **180** vs Momentum's 30

## 6. Where the port is ahead

`ble_spam`, `clock_app`, `doom`, `dualboot`, `esp_now`, `mp3_player`, `nrf24`,
`power_profiler`, `subghz_remote`, `wlan_app`, `wolf3d`, `snake_game`, `backup_settings`,
`namechanger`.

---

## Status / remaining order

**Done 2026-08-24:** 125 kHz LFRFID dropped from the build (no LF hardware on this board);
`pocsag`, `hormann_bisecur` and `x10` added and registered, bringing the SubGHz registry to
full parity with Momentum.

**Ruled out by the user — do not re-propose:** `ibutton`, `gpio`, `onewire`, `updater`. The
LilyGO board does not have the hardware.

Remaining, in order:

1. **Desktop settings** — the most-missed settings app (favourites, PIN, auto-lock); the
   component is already in the tree. Expect Left/Right input rework, and confirm it actually
   appears in the menu (a saved layout has hidden newly added apps before).
2. **HID remote** — `ble_hid` exists; check the input model before committing.
3. **`input_settings`, `clock_settings`, `system`** — small settings apps.
4. **Small settings fields** — `battery_icon`, `favorite_timeout`, `rpc_color_*`.

Weather-station decoders are deliberately NOT on this list; see section 2.
