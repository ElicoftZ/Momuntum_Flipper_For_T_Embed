#!/usr/bin/env python3
"""Regenerate firmware/manifest.json from the hosted firmware image.

The web flasher downloads the image hosted next to this manifest, so the
manifest -- not the GitHub release asset -- is its source of truth for size
and hash. Run this whenever firmware/*.bin changes, or the flasher will
measure the new image against a stale number.

The image itself may be any size; the checks here are structural.
"""
import hashlib
import json
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FW = ROOT / "firmware" / "momentum_t_embed_RELEASE.bin"
OUT = ROOT / "firmware" / "manifest.json"


def app_offset(image):
    """Offset of the factory app, read from the partition table at 0x8000."""
    table = image[0x8000:0x9000]
    for i in range(0, len(table), 32):
        entry = table[i:i + 32]
        if entry[:2] != b"\xaa\x50":
            break
        ptype, subtype = entry[2], entry[3]
        offset, _size = struct.unpack("<II", entry[4:12])
        if ptype == 0 and subtype == 0:
            return offset
    return None


def main():
    if not FW.is_file():
        sys.exit(f"missing {FW}")
    image = FW.read_bytes()

    if image[:1] != b"\xe9":
        sys.exit(f"{FW.name} does not start with the ESP image magic 0xe9")
    if image[0x8000:0x8002] != b"\xaa\x50":
        sys.exit(f"{FW.name} has no partition table at 0x8000")
    offset = app_offset(image)
    if offset is None:
        sys.exit(f"{FW.name} has no factory app partition")
    if offset >= len(image) or image[offset] != 0xE9:
        sys.exit(f"{FW.name} is truncated: no app image at {offset:#x}")

    manifest = {
        "name": FW.name,
        "path": f"firmware/{FW.name}",
        "size": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "flashAddress": "0x0",
        "flashSize": "16MB",
    }
    OUT.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"firmware/manifest.json: {manifest['size']} bytes, "
          f"app at {offset:#x}, sha256 {manifest['sha256'][:16]}...")


if __name__ == "__main__":
    main()
