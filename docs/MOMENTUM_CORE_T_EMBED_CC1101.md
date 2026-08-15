# Momentum Core for LilyGO T-Embed CC1101

This port milestone targets only the LilyGO T-Embed CC1101 (`lilygo_t_embed_cc1101`, ESP32-S3). It keeps the ESP32 port's display, rotary input, SD, USB, mesh, qFlipper, and radio HAL as the hardware foundation.

## Included in the core milestone

- Momentum-compatible persisted settings for animation pack, speed, cycle timing, unlock filtering, and future menu style selection.
- A T-Embed rotary-native **Momentum Settings** app for animation pack, speed, cycle timing, and unlock filtering.
- Settings load after the storage service is ready and before the desktop service starts.
- Animation lookup in `/ext/asset_packs/<pack>/Anims`; a missing pack manifest falls back to `/ext/dolphin`, while malformed pack data falls back to the compiled safe animation.
- External animation speed control, cycle override/off mode, unlock filtering, and no-immediate-repeat selection.
- Safe fallback for missing, malformed, zero-weight, or unreadable external animations.
- Safe freeze/resume of compressed external frames when opening and closing applications.

`menu_style` is persisted now for Momentum file compatibility. The nine Momentum menu renderers and their T-Embed rotary navigation are a later milestone after this core passes hardware testing.

## Momentum on the T-Embed

Open **Momentum** from the main menu; it sits at the bottom of the app list, matching upstream Momentum, which ships it as `applications/main/momentum_app` rather than as a settings entry. The core page contains only settings that are active in this milestone:

- **Asset Pack**: `Default` or a safe, alphabetically sorted directory containing `Anims/manifest.txt` under `/ext/asset_packs`.
- **Anim Speed**: Momentum's `25%` through `300%` presets.
- **Cycle Anims**: `OFF`, `Meta.txt`, or the `15 S` through `24 H` presets.
- **Unlock Anims**: bypasses animation mood and level restrictions when set to `ON`.

Rotate to move between rows. Click the encoder to open Asset Pack or enter a value's edit mode, rotate to change it, and click again to finish editing. The side button goes Back. Leaving the main Momentum page saves changed values and immediately selects a fresh desktop animation with a fresh cycle timer; no reboot is required.

Because both `/int` settings compatibility and `/ext` asset packs currently use the SD layer on this ESP32 port, the controls are locked with `SD card required` when no card is mounted. A configured pack that is temporarily missing is preserved until another pack or `Default` is explicitly selected.

## Settings file

The firmware reads `MOMENTUM_SETTINGS_PATH`, which is `/int/.momentum_settings.txt`. In this ESP32 port, internal-path compatibility is currently backed by the SD storage layer. Missing storage or a missing file uses safe defaults.

Example:

```text
asset_pack: Momentum
anim_speed: 100
cycle_anims: 0
unlock_anims: false
menu_style: 0
```

- `asset_pack`: empty for `/ext/dolphin`, or a single safe directory name under `/ext/asset_packs`.
- `anim_speed`: `25` through `300` percent; default `100`.
- `cycle_anims`: `-1` disables cycling, `0` uses each animation's metadata duration, and `1` through `86400` overrides the duration in seconds.
- `unlock_anims`: `true` bypasses mood and level filtering.
- `menu_style`: Momentum-compatible numeric value `0` through `8`; currently reserved for the next UI milestone.

For `asset_pack: Momentum`, the animation manifest must be at:

```text
/ext/asset_packs/Momentum/Anims/manifest.txt
```

Each named animation keeps the standard `meta.txt` and `frame_N.bm` layout below that `Anims` directory. Pack icons and fonts are deliberately not enabled in this milestone.

## Main menu styles

All nine Momentum styles are available under **Momentum > Interface > Menu Style**: `List`, `Wii`, `DSi`, `PS4`, `Vertical`, `C64`, `Compact`, `MNTM`, and `CoverFlow`. The change applies as soon as you leave the app; no reboot is required.

Navigation on the T-Embed's rotary encoder:

- Rotate for up/down.
- Hold the encoder and rotate for left/right.
- Click the encoder for OK, side button for Back.

That matters because the styles differ in which axis they use. `List`, `MNTM`, `C64` and `Compact` move on up/down. `DSi`, `PS4`, `Vertical` and `CoverFlow` are horizontal and move on left/right, so they need the held-encoder gesture. `Wii` is a grid and uses both.

`Vertical` rotates the canvas and honours the left-handed setting (`FuriHalRtcFlagHandOrient`), which swaps its left and right.

The `PS4` style shows the dolphin level, read through the public `dolphin_stats()` API rather than upstream's reach into `dolphin->state->data.icounter`. The `MNTM` style shows the device name, clock, battery percentage, charge state and 5 V OTG state.

Because the port renders a 128x64 logical framebuffer and aspect-fit scales it to the 320x170 LCD, the styles keep Momentum's pixel-exact layout rather than being re-laid out for the wider panel.

## Lockscreen

The locked screen is Momentum's: a cover that slides down over the display carrying a clock, date and unlock prompt, replacing the port's previous closing-doors screen. All eight settings under **Momentum > Interface > Lockscreen** are live:

- **Allow Poweroff** — hold Back on the lockscreen to power off.
- **Show Time**, **Show Seconds**, **Show Date** — clock uses big numbers, and honours the locale's 12/24-hour and date-order settings plus the `midnight_format_00` preference.
- **Show Statusbar** — keeps the status bar drawn while locked.
- **Unlock Prompt** — the speech bubble telling you how to unlock.
- **Transparent (see animation)** — skips the cover graphic so the desktop animation stays visible behind the clock.
- **Skip Sliding Animation** — locks and unlocks instantly.

Unlocking without a PIN is Back three times. **With a PIN set, rotate up to open PIN entry**; the prompt draws an up arrow to match. This is Momentum's behaviour and differs from the port's previous screen, where any key opened PIN entry.

## Icons

This port ships `components/assets/assets_icons.c` pre-generated and has no asset build step, so icons taken from upstream are encoded by `tools/png2icon.py`:

```bat
python tools\png2icon.py Lockscreen ..\Momentum-Firmware\assets\icons\Interface\Lockscreen.png
```

It emits the three C lines to append to `assets_icons.c`; add a matching `extern const Icon I_<name>;` to `assets_icons.h`.

Icons are written uncompressed — a `0x00` header byte followed by XBM rows — which is the path `compress_icon_decode()` takes when the header says the data is not compressed. That avoids a heatshrink dependency at the cost of some flash, which this 16 MB board has to spare.

The encoder only accepts 1-bit greyscale, non-interlaced PNG, which is what Flipper icons are; anything else is rejected rather than silently mis-encoded. Verify it against the icons already in the tree with:

```bat
python tools\png2icon.py --self-test
```

## Adding a Momentum setting

Every persisted setting is described once, in the `momentum_settings_entries` table in `lib/momentum/settings_core.c`. Defaults, range clamping, change detection, loading and saving all iterate that table, so adding a setting is two edits:

1. Add the field to `MomentumSettings` in `lib/momentum/settings_core.h`.
2. Add one `ENTRY_*` row to `momentum_settings_entries`, then bump `MOMENTUM_SETTINGS_ENTRY_COUNT`.

Pick the row type by how the value should behave when a file holds something out of range:

- `ENTRY_UINT` / `ENTRY_INT` clamp to the nearest limit, because a number is still meaningful there.
- `ENTRY_ENUM` falls back to the default, because no enum case sits outside its range and clamping would silently pick a neighbouring one.
- `ENTRY_BOOL` and `ENTRY_STR` need no range.

Only `ENTRY_INT` is signed. Values are carried as `int64_t` so a large `uint32_t` cannot come back negative and clamp to its minimum instead of its maximum.

Fields are addressed by offset rather than by pointer, so the table applies to any `MomentumSettings` instance — the global one, a previous copy for change detection, or an app's working copy.

The settings file stays Momentum-compatible: keys match upstream names, so a `.momentum_settings.txt` written by Momentum keeps the values this port understands and ignores the rest.

## Host tests

`lib/momentum/settings_core.c` has no furi or ESP-IDF dependencies, so its logic runs on the host. With the MSVC build tools installed:

```bat
tests\host\run_host_tests.bat
```

The tests cover defaults, sanitizing, asset pack name safety, animation speed and cycle maths, and the table itself: unique keys, no overlapping fields, in-range defaults, and that change detection compares every entry. They do not need hardware or an SD card.

## Build one merged image on Windows

Run from the repository root:

```bat
build_merged_t_embed_cc1101.bat
```

The result is:

```text
build_t_embed\momentum_t_embed_cc1101_merged.bin
```

To build and immediately flash a connected board, pass its serial port:

```bat
build_merged_t_embed_cc1101.bat COM14
```

The script is fixed to ESP32-S3, 16 MB flash, and `lilygo_t_embed_cc1101`; it does not offer or build another board. CMake also rejects any other `FLIPPER_BOARD` while this target-specific port is active.

## Hardware smoke test before the next port phase

1. Boot once without an SD card and confirm the desktop uses its compiled animation without a reset or watchdog.
2. Boot with a valid `/ext/dolphin` set and confirm rotary up/down, held-encoder left/right, OK, and Back still work.
3. Open **Settings > Momentum Settings**, select an asset pack, and confirm its animation is used immediately after Back; rename the manifest and confirm automatic fallback.
4. Test animation speeds `25`, `100`, and `300` with a known 4 FPS animation; expected rates are 1, 4, and 12 FPS.
5. Change cycle timing from `24 H` to `15 S` and confirm the first post-settings cycle uses 15 seconds; then select `OFF` and confirm cycling stops.
6. Use a manifest whose usable weights are all zero and confirm there is no divide-by-zero reset.
7. Remove and reinsert the SD card, then open and close Settings, qFlipper, USB storage, and mesh repeatedly.
8. Launch a CC1101/Sub-GHz operation after desktop animation activity to check the shared SPI bus remains healthy.
9. Repeat at least 20 app enter/exit cycles and run the desktop animation for 30 minutes while watching reset reason and heap watermark.

Proceed to menu styles, lockscreen/status-bar customization, icon/font packs, and the broader Momentum feature set only after these checks pass on the physical T-Embed CC1101.
