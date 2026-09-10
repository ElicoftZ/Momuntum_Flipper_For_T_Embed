# ARM FAP and Flipper Lab validation — 2026-09-05

## Implemented

- RPC device-info advertises `firmware.target=7`, `firmware.api.major=88`,
  `firmware.api.minor=2` in both legacy and property responses. Lab uses these
  fields to select a catalog package. Local/native target 32 and the native
  Xtensa API table remain unchanged. Actual firmware version remains 1.4.3.
- The ARM loader accepts API 87.0–87.1 and 88.0–88.2 with the existing import
  whitelist. It rejects future profiles, unsupported imports and relocations.
  ARM load errors now expose the specific diagnostic to the Apps dialog.
- The user CSV is preserved in `tests/fixtures/arm_api`. Its SHA-256 matches the
  official development SDK. The audit records all 2,741 enabled symbols:
  32 bridged, 718 native-export candidates, 1,991 without an ARM bridge.
  Disabled rows and header declarations are not counted as supported APIs.
- The launcher keeps desktop animations paused until both the app browser and
  its parent launcher menu close. The animation semaphore waits for its actual
  acknowledgement after logging a slow operation instead of asserting at three
  seconds. This addresses the previously captured Apps-reopen crash.

## Verified on the host

- Public catalog rejects `f32 / 0.1`; accepts `f7 / 87.1` and `f7 / 88.2`.
  API 88.2 belongs to the 1.5.0-rc SDK, whereas 1.4.3 is the latest stable
  firmware release returned by GitHub during this check.
- Original repository RPS binary: 5,416 bytes, API 87.1,
  SHA-256 `550e2bc478cb6bf7da2ec18fbb8737fcf24031f2a01afbb1d2b2b1a47263175c`.
- Original catalog RPS binary: 5,412 bytes, API 88.2,
  SHA-256 `3caee16c0ed537bead531b44ddd98158d424d7d4732ae5aa2fc5e83ac0a651e4`.
- Both binaries produce identical 357-call traces in the actual C interpreter
  and independent Unicorn execution. Tests cover all three sprites, animation,
  two rounds, OK replay, ignored Up/Down and release events, and Back exit.
- 2,464 instruction/register/flag comparisons pass, along with malformed ELF,
  memory protection, allocation reuse, execution-budget and opcode rejection.
- Actual C RPC identity helper tested with both separators; hardware, version,
  protobuf and power fields retain their incoming values. Missing ARM imports
  still fail under API 88.2. All 32 bridge declarations match the 87.1 SDK.
- Firmware build and partition-size checks pass. Logs:
  `build_host/lab_api_build_final.log` (RPC),
  `build_host/lab_api_build_verified.log` (final),
  `build_host/lab_api_host_tests.log`.

## Flashed hardware

App-only firmware flashed to ESP32-S3 COM4 at offset `0x20000`; esptool verified
the written data. Partitions and SD files were not rewritten by this flash.

- Binary: `build_t_embed_release/furi_esp32.bin`, 3,304,560 bytes.
- SHA-256: `fa1fa69b1aa27eed698397350de20d593af1704e3bb3676be9244d39d517a8ac`.
- Boot completed: internal free 21,959 bytes, largest free block 13,824 bytes.
- Guest arena remains 64 KiB in PSRAM, with the existing foreground stack reserve.
  The complete SDK inventory is a host report, not a resident firmware table.
- Flash log: `build_host/lab_api_flash.log`.
- Serial log: `build_host/lab_api_hardware.log`.

The user subsequently reported a freeze launching catalog `pixel_rps`, and a
folder stuck on Loading even without Lab connected. A fresh native-USB capture
(`build_host/lab_folder_freeze.log`) ends at `Invalid ELF magic` during metadata
loading. No panic or reboot was captured. The installed file's contents have
not yet been retrieved, so a corrupt download is a hypothesis, not a finding.

## Folder rejection fix under validation

- Failed native ELF opens now release their file handle and parse buffers before
  returning. Only a recognized little-endian, 32-bit, relocatable ARM ELF can
  enter the interpreter fallback; other rejected files return Invalid file.
- Invalid-header diagnostics now include the path and four header bytes.
- Firmware build/partition checks pass; the existing interpreter regression
  suite passes for both original RPS binaries, including 2,464 instruction/flag
  comparisons. These host tests do not exercise native storage or GUI locking.
- Candidate binary: 3,304,688 bytes, SHA-256
  `7c4dbcbbef015a0155f04f093de97d194d9995af7b6ae695f3353e5315b4263c`.
- Build log: `build_host/lab_folder_fix_build.log`; host test log:
  `build_host/lab_folder_fix_host_tests.log`.
- This candidate has **not been flashed or verified on hardware**. USB became
  unavailable; the next step is to inspect the SD files in USB Storage, then
  flash and retest the affected folder and repeated Lab launches. The freeze
  must not be marked resolved on the strength of these host checks.

The subsequent Morse Trainer diagnostic build includes this guard and was
flashed successfully. Its hash and test status are recorded in
`ARM_FAP_MORSE_TRAINER.md`; the affected folder still needs a hardware retest.

## Scope

This is a working subset for an unchanged ARM app, not compatibility with all
FAPs. The symbol CSV supplies declarations, not the implementations, structure
layouts, CPU emulation or peripheral hardware needed by other apps. Next bridge
groups and the complete per-symbol inventory are in `arm_fap_api_coverage.md`
and `arm_fap_api_coverage.csv`. T-Embed input remains Up/Down/OK/Back.

Sources: [Lab installation source](https://github.com/flipperdevices/lab.flipper.net/blob/2e8e494bcd3704f93a15868eae04c01170eff463/frontend/src/pages/Apps.vue),
[official development API](https://github.com/flipperdevices/flipperzero-firmware/blob/7f0b6e1c14431708cfde75ae1ba13df59e868041/targets/f7/api_symbols.csv),
[official stable release](https://github.com/flipperdevices/flipperzero-firmware/releases/tag/1.4.3).
