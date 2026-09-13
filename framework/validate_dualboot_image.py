#!/usr/bin/env python3
"""
validate_dualboot_image.py — check a firmware .bin against the exact rules
this project's Dual Boot installer enforces, BEFORE you try to install it
on a device.

This mirrors dualboot_locate_image() in
applications/main/dualboot/dualboot_app.c byte-for-byte: same header
offsets, same magic numbers, same segment walk, same size math. If this
script accepts your file, the on-device installer will too (structurally —
it still separately checks that the image fits in whatever free pool space
that specific device currently has, which this script cannot know).

If this script rejects your file, the device would have shown
"Recovery blocked boot" with the same message this script prints, and left
Flipper/Momentum selected and untouched. Nothing on a device is at risk by
running this script; it only reads your local file.

Usage:
    python3 validate_dualboot_image.py path/to/firmware.bin

Accepts two shapes, same as the on-device installer:
  1. A raw app image — plain output of `esptool.py elf2image`, or the
     `.bin` an ESP-IDF build produces before merging. Simplest, recommended.
  2. A merged/full image — output of `esptool.py merge_bin` (or
     `idf.py build` + merge), which carries a real ESP-IDF partition table
     at offset 0x8000. The script scans that table for the first APP-type
     entry and validates the image found there, exactly like the device
     does.
"""
import struct
import sys

# ---- esp_image_header_t (24 bytes, packed) ---------------------------------
# offset  size  field
#   0      1    magic            (must be 0xE9)
#   1      1    segment_count
#   2      1    spi_mode
#   3      1    spi_speed/spi_size (packed nibbles, unchecked here)
#   4      4    entry_addr
#   8      1    wp_pin
#   9      3    spi_pin_drv[3]
#  12      2    chip_id          (little-endian)
#  14      1    min_chip_rev
#  15      2    min_chip_rev_full
#  17      2    max_chip_rev_full
#  19      4    reserved[4]
#  23      1    hash_appended
IMAGE_HEADER_FMT = "<BBBBIB3sHBHH4sB"
IMAGE_HEADER_SIZE = struct.calcsize(IMAGE_HEADER_FMT)
assert IMAGE_HEADER_SIZE == 24, IMAGE_HEADER_SIZE

ESP_IMAGE_HEADER_MAGIC = 0xE9
# ESP-IDF's esp_app_format.h chip id for ESP32-S3. Only chip this project's
# recovery bootloader and multiboot layout are validated against
# (components/multiboot/multiboot.c gates on esp32s3 hardware). If your IDF
# version ever changes this constant, esptool's own output will still be
# authoritative — cross-check against esp_app_format.h in your SDK if this
# script's rejection looks wrong for a genuine ESP32-S3 build.
ESP_CHIP_ID_ESP32S3 = 0x0009

# ---- esp_image_segment_header_t (8 bytes, packed) ---------------------------
SEGMENT_HEADER_FMT = "<II"  # load_addr, data_len
SEGMENT_HEADER_SIZE = struct.calcsize(SEGMENT_HEADER_FMT)
assert SEGMENT_HEADER_SIZE == 8

# ---- ESP-IDF partition-table binary entry (32 bytes, packed) ---------------
# Same format esp-idf itself writes at 0x8000 in a real device image.
# offset  size  field
#   0      2    magic   (0x50AA for a real entry)
#   2      1    type
#   3      1    subtype
#   4      4    offset
#   8      4    size
#  12     16    label
#  28      4    flags
PART_ENTRY_FMT = "<HBBII16sI"
PART_ENTRY_SIZE = struct.calcsize(PART_ENTRY_FMT)
assert PART_ENTRY_SIZE == 32

DUALBOOT_PART_TABLE_OFFSET = 0x8000
DUALBOOT_PART_TABLE_MAGIC = 0x50AA
DUALBOOT_PART_TYPE_APP = 0x00
DUALBOOT_PART_ENTRY_MAX = 95

# This project's dynamic multiboot pool (components/multiboot/layout.h).
# The on-device installer also needs a free gap this large or larger in
# whatever is already installed — this script only knows the theoretical
# ceiling, not any specific device's current free space.
MB_POOL_START = 0x920000
MB_POOL_END = 0xf20000
MB_POOL_TOTAL = MB_POOL_END - MB_POOL_START
MB_ALIGNMENT = 0x10000  # 64 KiB — installs round up to this


def read_header(data, at):
    if at + IMAGE_HEADER_SIZE > len(data):
        return None
    fields = struct.unpack_from(IMAGE_HEADER_FMT, data, at)
    return {
        "magic": fields[0],
        "segment_count": fields[1],
        "spi_mode": fields[2],
        "chip_id": fields[7],
        "hash_appended": fields[12],
    }


def locate_image(data):
    """Mirrors dualboot_locate_image() exactly. Returns (offset, length) or
    raises ValueError with the same message the device would show."""
    size = len(data)
    hdr = read_header(data, 0)
    if hdr is None:
        raise ValueError("File too small")
    if hdr["magic"] != ESP_IMAGE_HEADER_MAGIC:
        raise ValueError("Not a firmware image")

    offset = 0
    extracted = False
    if size > DUALBOOT_PART_TABLE_OFFSET + PART_ENTRY_SIZE:
        pos = DUALBOOT_PART_TABLE_OFFSET
        for _ in range(DUALBOOT_PART_ENTRY_MAX):
            if pos + PART_ENTRY_SIZE > size:
                break
            magic, type_, subtype, part_offset, part_size, label, flags = (
                struct.unpack_from(PART_ENTRY_FMT, data, pos)
            )
            pos += PART_ENTRY_SIZE
            if magic != DUALBOOT_PART_TABLE_MAGIC:
                break
            if type_ != DUALBOOT_PART_TYPE_APP or part_offset >= size:
                continue
            offset = part_offset
            extracted = True
            break

    hdr = read_header(data, offset)
    if hdr is None:
        raise ValueError("Truncated image")
    if hdr["magic"] != ESP_IMAGE_HEADER_MAGIC:
        raise ValueError("No app inside image")
    if hdr["chip_id"] != ESP_CHIP_ID_ESP32S3:
        raise ValueError(
            f"Wrong device: not ESP32-S3 (chip_id=0x{hdr['chip_id']:04x}, "
            f"expected 0x{ESP_CHIP_ID_ESP32S3:04x})"
        )
    if hdr["segment_count"] == 0 or hdr["segment_count"] > 16:
        raise ValueError(f"Corrupt image header (segment_count={hdr['segment_count']})")

    end = offset + IMAGE_HEADER_SIZE
    for i in range(hdr["segment_count"]):
        if end + SEGMENT_HEADER_SIZE > size:
            raise ValueError(f"Truncated segment header (segment {i})")
        load_addr, data_len = struct.unpack_from(SEGMENT_HEADER_FMT, data, end)
        end += SEGMENT_HEADER_SIZE + data_len
        if end > size:
            raise ValueError(f"Truncated segment data (segment {i})")

    end = offset + ((end - offset + 16) & ~15)
    if hdr["hash_appended"]:
        end += 32
    if end > size:
        raise ValueError("Truncated / oversized app")

    length = end - offset
    if length > MB_POOL_TOTAL:
        raise ValueError(
            f"Truncated / oversized app: {length} bytes exceeds the entire "
            f"pool ({MB_POOL_TOTAL} bytes) — this can never install, "
            f"regardless of free space"
        )

    return offset, length, extracted, hdr


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} path/to/firmware.bin", file=sys.stderr)
        return 2

    path = sys.argv[1]
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        print(f"FAIL: cannot read {path}: {e}", file=sys.stderr)
        return 2

    try:
        offset, length, extracted, hdr = locate_image(data)
    except ValueError as e:
        print(f"FAIL: {e}")
        print()
        print("This is the same message and outcome the on-device Dual Boot")
        print("installer would show ('Recovery blocked boot') — Flipper/")
        print("Momentum stays selected and nothing on the device changes.")
        return 1

    aligned = (length + MB_ALIGNMENT - 1) & ~(MB_ALIGNMENT - 1)
    print("PASS: structurally installable")
    print(f"  shape:          {'merged image (extracted app)' if extracted else 'raw app image'}")
    print(f"  app offset in file: 0x{offset:x}")
    print(f"  app length:     {length} bytes ({length / 1024:.1f} KiB)")
    print(f"  rounds up to:   {aligned} bytes ({aligned / 1024:.0f} KiB) at 64 KiB alignment")
    print(f"  segment_count:  {hdr['segment_count']}")
    print(f"  spi_mode byte:  {hdr['spi_mode']} (0=QIO 1=QOUT 2=DIO 3=DOUT — informational; "
          f"the installer does not check this, see framework/README.md)")
    print()
    print(f"  Pool capacity on a freshly-flashed device: {MB_POOL_TOTAL} bytes "
          f"({MB_POOL_TOTAL / 1024 / 1024:.1f} MiB) total, shared by every installed slot.")
    print("  Actual free space on any given device depends on what else is")
    print("  already installed there — this script cannot know that; the")
    print("  on-device installer will still refuse the file if it doesn't fit.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
