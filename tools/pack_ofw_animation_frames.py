#!/usr/bin/env python3
"""Replace source PNGs in an SD-card ZIP with loadable Flipper BM frames.

The firmware deliberately keeps runtime image decoding small and reads desktop
animation frames as ``frame_N.bm``. The assembled SD-card source contains
``frame_N.png`` files and a Momentum 30-level manifest, so copying that ZIP
unchanged makes every external animation fail and leaves the compiled TV
fallback on screen.

This converter writes a separate output ZIP and never overwrites its input.
It installs the official 3-level manifest, converts its listed animation
frames, drops non-OFW animation folders, and copies all other SD content
unchanged.

Official manifest source:
https://github.com/flipperdevices/flipperzero-firmware/blob/dev/assets/dolphin/external/manifest.txt
"""

from __future__ import annotations

import argparse
import copy
import pathlib
import re
import tempfile
import zipfile

from png2icon import png_to_icon


FRAME_RE = re.compile(r"^dolphin/[^/]+/frame_[0-9]+\.png$")
DEFAULT_MANIFEST = pathlib.Path(__file__).with_name("ofw_dolphin_manifest.txt")


def read_manifest(path: pathlib.Path) -> tuple[bytes, set[str]]:
    payload = path.read_bytes()
    names = {
        line.partition(":")[2].strip()
        for line in payload.decode("utf-8").splitlines()
        if line.startswith("Name:")
    }
    if not names:
        raise ValueError(f"no animation names in {path}")
    return payload, names


def convert_zip(
    source: pathlib.Path, output: pathlib.Path, manifest: pathlib.Path
) -> tuple[int, int, int]:
    source = source.resolve()
    output = output.resolve()
    if source == output:
        raise ValueError("input and output must be different paths")
    if not source.is_file():
        raise FileNotFoundError(source)

    manifest_payload, official_animations = read_manifest(manifest)
    output.parent.mkdir(parents=True, exist_ok=True)
    converted = 0
    dropped = 0
    replaced_manifest = False

    with tempfile.TemporaryDirectory(prefix="ofw-animation-") as temp_dir:
        scratch_png = pathlib.Path(temp_dir) / "frame.png"

        with zipfile.ZipFile(source, "r") as source_zip, zipfile.ZipFile(
            output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
        ) as output_zip:
            for info in source_zip.infolist():
                payload = source_zip.read(info)
                if info.filename == "dolphin/manifest.txt":
                    manifest_info = copy.copy(info)
                    output_zip.writestr(manifest_info, manifest_payload)
                    replaced_manifest = True
                    continue

                parts = pathlib.PurePosixPath(info.filename).parts
                animation_name = (
                    parts[1]
                    if len(parts) >= 3 and parts[0] == "dolphin"
                    else None
                )
                if animation_name and animation_name not in official_animations:
                    dropped += 1
                    continue

                if info.is_dir() or not FRAME_RE.fullmatch(info.filename):
                    output_zip.writestr(info, payload)
                    continue

                scratch_png.write_bytes(payload)
                _, _, bitmap = png_to_icon(scratch_png)

                bitmap_info = copy.copy(info)
                bitmap_info.filename = info.filename[:-4] + ".bm"
                bitmap_info.compress_type = zipfile.ZIP_DEFLATED
                output_zip.writestr(bitmap_info, bitmap, compresslevel=9)
                converted += 1

    if not replaced_manifest or converted == 0:
        output.unlink(missing_ok=True)
        raise ValueError("source ZIP is missing its manifest or animation frames")

    with zipfile.ZipFile(output, "r") as check_zip:
        names = check_zip.namelist()
        remaining_png = [name for name in names if FRAME_RE.fullmatch(name)]
        bitmap_count = sum(
            1
            for name in names
            if name.startswith("dolphin/") and "/frame_" in name and name.endswith(".bm")
        )
        output_animations = {
            pathlib.PurePosixPath(name).parts[1]
            for name in names
            if name.startswith("dolphin/") and name.endswith("/meta.txt")
        }
        if (
            remaining_png
            or bitmap_count != converted
            or output_animations != official_animations
            or check_zip.read("dolphin/manifest.txt") != manifest_payload
        ):
            output.unlink(missing_ok=True)
            raise ValueError("output ZIP verification failed")

    return converted, dropped, len(official_animations)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path, help="source SD-card ZIP")
    parser.add_argument("output", type=pathlib.Path, help="new converted ZIP")
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=DEFAULT_MANIFEST,
        help=f"official manifest (default: {DEFAULT_MANIFEST})",
    )
    args = parser.parse_args()

    converted, dropped, animations = convert_zip(args.source, args.output, args.manifest)
    print(
        f"packed {animations} OFW animations ({converted} BM frames, "
        f"{dropped} non-OFW entries dropped) -> {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
