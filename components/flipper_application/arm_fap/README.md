# ARM FAP compatibility: first implementation

This runtime executes the unchanged ARM binary from
[xantopren/rock-paper-scissors](https://github.com/xantopren/rock-paper-scissors),
commit `fffa4ea5233e1c56c18952b357ab34f409a9cf7d`. Its source is a test reference,
not a native build input. The original MIT license and FAP are in
`tests/fixtures/arm_fap_rps/`. The native Xtensa FAP path remains separate.

The portable core loads little-endian ARM ELF32 relocatable files, maps allocated
sections into guest memory, resolves supported firmware imports to interpreter
traps and applies standard `R_ARM_ABS32` relocations. It implements the Thumb and
Thumb-2 instruction subset exercised by this binary. Unsupported instructions,
imports, relocations and out-of-bounds accesses stop the app with an error.

The ABI profiles accept API 87.0/87.1 and 88.0/88.1/88.2, the 85-byte Flipper manifest and
its zero-flags Momentum extension. It exposes 39 APIs for memory, random values,
the GUI record, views, a dispatcher and drawing. Limits are one dispatcher, four
views, 32 arena allocations, single-frame icons up to 128 × 64, and 100,000
instructions per guest entry/callback. FAP plugins, guest threads, FPU/DSP
instructions, additional hardware APIs, and fast-relocation-only packages are
not implemented. This is not general ARM or catalog compatibility yet.

The memory/string imports include `memcpy`, `memmove`, `memset`, `strlen`,
`strcmp`, `strlcpy` and `strlcat`. They validate guest read/write ranges before
accessing bytes and allocate no persistent buffers. String sources use the
existing 1,024-byte scan limit, including the terminating NUL. `memmove` permits
overlap; undefined overlapping `memcpy` and string-copy calls are rejected.
Size-bounded string copies return the attempted length so apps can detect
truncation. Empty memory operations do not dereference their pointers.

Guest RAM is a 64 KiB PSRAM allocation, including an 8 KiB guest stack; the
interpreter and GUI bridge state also live in PSRAM. The native execution thread
uses the existing 16 KiB internal foreground stack reserve. Control blocks and
hardware DMA remain native. All app resources are released at exit.

Guest addresses and opaque GUI handles never become callable native pointers.
The bridge marshals ARM short-enum input events, native view handles, models,
callbacks, stack arguments and compressed icon frames explicitly. T-Embed's
Up/Down/OK/Back events retain those meanings. Native GUI operations release the
interpreter mutex before taking the GUI lock; nested callbacks save and restore
the suspended guest CPU state. Guest model operations are serialized within
this single-thread/callback profile.

## Validation

Run `tests/host/build_arm_fap_test.bat`, then `python tests/host/test_arm_fap.py`
with pyelftools, Capstone and Unicorn installed. The test runs the original FAP
through a mock GUI with the actual C interpreter and with Unicorn, comparing
the full import trace. It exercises animation, all three sprites, controls,
ignored inputs while animating and exit. It also compares instruction flags and
registers over randomized operands and checks malformed input and execution
limits. Native GUI behavior must additionally be checked on T-Embed.

The memory/string tests call actual interpreter import traps, checking return
values, unsigned comparisons, moves in both overlap directions, 256 randomized
string truncation/guard cases and rejection without partial writes. Run
`python tools/inspect_arm_fap.py path/to/app.fap` to extract the manifest and
missing imports without executing a FAP. Directories and `--output report.json`
are supported. Source code must still be obtained from its author; the tool
extracts symbols, not original C code or a guarantee of compatibility.

## Flipper Lab catalog identity

RPC device-info now reports firmware target `7` and API `88.2`, for both the
legacy underscore keys and dotted `devinfo` properties. These are the fields
Flipper Lab uses to request catalog builds. The firmware version remains
`1.4.3`; changing that string alone does not fix catalog selection. Native
Xtensa target/API validation and local hardware identity are unchanged. The
ESP32 RPC firmware-update/reboot commands remain unimplemented.

The public catalog accepted `f7 / 88.2` and rejected `f32 / 0.1` on 2026-09-05.
API 88.2 is the catalog's `1.5.0-rc` SDK, not the latest stable firmware release.
The original catalog `pixel_rps` binary is in `tests/fixtures/arm_fap_rps_lab`.
It passes the same C-versus-Unicorn replay/exit test as the repository binary.
The test also checks both RPC naming schemes, future-profile rejection and
missing-import rejection. This is host verification; an actual website install
and launch on T-Embed still need hardware validation after flashing this build.

The identity enables catalog selection; it does not implement missing APIs.
Unsupported imports still fail before execution, with the specific ARM error
shown in the loader. The user's API CSV contains 2,741 enabled functions and
variables. `python tools/audit_arm_fap_api.py` checks all bridged declarations
against API 87.1 and writes `docs/arm_fap_api_coverage.csv` and `.md`. The complete
inventory stays on the host, adding no firmware RAM. Native symbols listed as
bridge candidates still need guest pointers, structures, callbacks, handles
and cleanup adapted explicitly.

Reference source: [Flipper Lab catalog selection and installation](https://github.com/flipperdevices/lab.flipper.net/blob/2e8e494bcd3704f93a15868eae04c01170eff463/frontend/src/pages/Apps.vue).
