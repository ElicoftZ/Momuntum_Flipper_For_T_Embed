#!/usr/bin/env python3
"""Pack the SD content this port ADDS into a tar for the `assets` partition.

Not a whole card image. Someone flashing this firmware already has an ordinary
Flipper SD layout; only the delta this port introduces has to travel in flash:
its own .fap apps, the welcome slideshow, and the folders its apps expect to
exist. That keeps the payload around 320 KB instead of 7.5 MB.

The first entry written is always `version.txt`. The boot-time extractor
compares it against the copy on the card and skips everything when they match,
which is what stops a re-extract on every boot.

Usage:
    python tools/pack_assets.py [--out build_t_embed/assets.tar] [--version STR]
"""

import argparse
import io
import os
import sys
import tarfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SDCARD = os.path.join(ROOT, "sdcard")

# What this port adds. Anything a stock card already has stays off this list.
INCLUDE_TREES = [
    "apps",           # this port's own .fap apps
    "dolphin/firstboot.bin",  # welcome slideshow
]

# Created empty so the apps have somewhere to write on a fresh card.
EMPTY_DIRS = [
    "apps",
    "apps_data",      # mic_logger writes its detection log here
    "voice_notes",    # voice_notes recordings
    "backup",         # Backup Settings
    "backup/nvs",
]

# Never ship these: editor leftovers, OS junk, and the stray duplicate tree.
EXCLUDE_NAMES = {".DS_Store", "Thumbs.db", "desktop.ini"}
EXCLUDE_TREES = {"dolphin - Copy"}


def should_skip(rel: str) -> bool:
    parts = rel.replace("\\", "/").split("/")
    if any(p in EXCLUDE_TREES for p in parts):
        return True
    return parts[-1] in EXCLUDE_NAMES


def add_bytes(tar: tarfile.TarFile, name: str, payload: bytes, mtime: int) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(payload)
    info.mtime = mtime
    info.mode = 0o644
    tar.addfile(info, io.BytesIO(payload))


def add_dir(tar: tarfile.TarFile, name: str, mtime: int) -> None:
    info = tarfile.TarInfo(name.rstrip("/") + "/")
    info.type = tarfile.DIRTYPE
    info.mode = 0o755
    info.mtime = mtime
    tar.addfile(info)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build_t_embed", "assets.tar"))
    ap.add_argument("--version", default=None,
                    help="version string written to version.txt (default: date-based)")
    ap.add_argument("--max-size", type=int, default=2 * 1024 * 1024,
                    help="assets partition size; packing more is a hard error")
    args = ap.parse_args()

    version = args.version or time.strftime("momentum-%Y%m%d")
    mtime = int(time.time())

    if not os.path.isdir(SDCARD):
        print("ERROR: %s not found" % SDCARD, file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    files = 0
    with tarfile.open(args.out, "w", format=tarfile.USTAR_FORMAT) as tar:
        # First entry, so the extractor can read it without scanning the whole
        # archive when it only needs to answer "is this already installed".
        add_bytes(tar, "version.txt", (version + "\n").encode(), mtime)

        for d in EMPTY_DIRS:
            add_dir(tar, d, mtime)

        for entry in INCLUDE_TREES:
            src = os.path.join(SDCARD, entry.replace("/", os.sep))
            if os.path.isfile(src):
                with open(src, "rb") as fh:
                    add_bytes(tar, entry, fh.read(), mtime)
                files += 1
                continue
            if not os.path.isdir(src):
                print("  skip (missing): %s" % entry)
                continue
            for dirpath, dirnames, filenames in os.walk(src):
                dirnames[:] = [d for d in dirnames if d not in EXCLUDE_TREES]
                for fn in sorted(filenames):
                    full = os.path.join(dirpath, fn)
                    rel = os.path.relpath(full, SDCARD).replace(os.sep, "/")
                    if should_skip(rel):
                        continue
                    with open(full, "rb") as fh:
                        add_bytes(tar, rel, fh.read(), mtime)
                    files += 1

    size = os.path.getsize(args.out)
    print("packed %d files -> %s" % (files, args.out))
    print("version.txt = %s" % version)
    print("size %d bytes (%.0f%% of the %d KB partition)"
          % (size, 100.0 * size / args.max_size, args.max_size // 1024))

    if size > args.max_size:
        print("ERROR: payload exceeds the assets partition", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
