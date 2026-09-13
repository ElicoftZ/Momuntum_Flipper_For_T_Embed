# Dual Boot porting framework

Everything a third-party/ported firmware needs to install cleanly through
this project's **Dual Boot** feature, without hand-tracing
`applications/main/dualboot/dualboot_app.c` and
`components/multiboot/multiboot.c` yourself. This folder is documentation
and a validator — it is not built into the firmware image; nothing here
runs on the device.

If you're porting a firmware and want to test it via Dual Boot instead of
flashing it directly, read this first. The most common way this goes
wrong is flashing a ported firmware's own partition table straight onto a
device that already has this project's multiboot layout — that replaces
the *entire* partition table, silently deleting the recovery partitions
(`partbackup`/`partpending`) and often `coredump` too, and breaks Dual
Boot on that device until it's reflashed from scratch. Everything below
exists to avoid that.

## The two ways to ship your firmware

**1. A raw app image (recommended).** The plain output of
`esptool.py elf2image`, or the `.bin` your build produces before any
merge step. Nothing but your app's own code — no bootloader, no
partition table. This is what the Dual Boot installer expects by default,
and it's the only shape that can never touch anything else on the device.

**2. A merged/full image.** Output of `esptool.py merge_bin` (bootloader +
partition table + app in one file, laid out at their normal absolute
offsets). The installer (`dualboot_locate_image()`) will look for a real
ESP-IDF partition table at offset `0x8000` inside your file, scan it for
the first APP-type entry, and pull just that app out — everything else in
the merged file (your bootloader, your partition table) is read but never
written anywhere. This exists for convenience if a merged image is all
your build produces; it costs you nothing extra over shape 1 and is
exactly as safe, but shape 1 is simpler to reason about.

**Never flash a merged image, or a partition table, directly with
esptool/a flash tool onto a device that has this project's dualboot
layout on it.** That bypasses the installer entirely and overwrites the
real partition table — see the warning above. The installer is the only
safe path once a device is running the multiboot layout.

## Hard requirements the installer checks

From `dualboot_locate_image()` — get any of these wrong and the device
shows **"Recovery blocked boot"**, prints your specific error, and leaves
Flipper/Momentum selected and untouched. Nothing is destructive if this
fails; it just refuses to install.

| Check | Requirement |
|---|---|
| Header magic | Standard ESP-IDF app image (`0xE9`) |
| Target chip | `ESP32-S3` — the only chip this project's recovery bootloader and multiboot layout support at all |
| Segment count | 1–16, and every segment must fit inside the file |
| Total size | Must fit within whatever free pool space the device currently has (6 MiB pool, minus whatever's already installed there) |

Run `validate_dualboot_image.py` (below) against your built `.bin` to check
all of this locally, before ever touching a real device.

## Soft requirement the installer does *not* check, but you should still meet

**Build your firmware as DIO flash mode.** Copy
`sdkconfig.defaults.dualboot` into your project (or set the equivalent
options in `idf.py menuconfig` → Serial Flasher Config) before building.

Nothing in the installer inspects your image's declared flash mode — a
QIO-built image will install without complaint. Whether it *boots*
afterward is a separate question: this project's own bootloader is built
DIO, and ESP-IDF's bootloader reconfigures flash-read timing per-app from
each app's own header right before jumping into it — but it can only
electrically use QIO if the flash chip's Quad-Enable status bit is
already set, which only happens if a bootloader was built with
`CONFIG_ESPTOOLPY_FLASHMODE_QIO` and ran once. This project's bootloader
never does that. Whether your specific chip's QE bit happens to already
be set from some earlier firmware (LilyGO's own reference config for this
board is QIO, so it's plausible) is not something this framework — or
anything running on the device — can tell you in advance. DIO needs no
such bit and is guaranteed to work regardless of that history. Match it
and this entire class of "installs fine, won't boot" failure doesn't
exist for you.

## Sizing

The installable pool is fixed at 6 MiB (`0x920000`–`0xf20000`,
`components/multiboot/layout.h`), shared across every slot installed on a
device — yours plus whatever else is already there. Installs round up to
64 KiB alignment. There's no way to know a specific device's *actual*
free space from outside it; the installer will refuse to install if your
image doesn't fit, same as any other check on this list.

## Using the validator

```
python3 framework/validate_dualboot_image.py path/to/your_firmware.bin
```

Exits 0 and prints a structural summary (offset, size, segment count) on
success; exits 1 and prints the exact message the device would have shown
on failure. It only reads your local file — nothing it does touches a
device, and there's no network or write access involved.

## Actually installing it

Once your image passes validation and is built DIO:

1. Copy the `.bin` onto the device's SD card.
2. On the device: **Dual Boot → Install → SD file**, pick it.
3. The installer stages the change (`multiboot_install`) and asks for a
   restart to apply the new layout — this two-step is how the recovery
   bootloader guarantees an interrupted install can never leave the
   partition table half-written (`bootloader_components/multiboot_recovery/recovery.c`).
4. After restart, select it from **Dual Boot → Boot firmware**.

None of this touches `coredump`, `partbackup`, `partpending`, or any
other installed slot — that's the entire point of going through the
installer instead of flashing a partition table directly.
