#!/usr/bin/env python3
"""Pack a directory of slideshow frames into the .bin the desktop reads.

Momentum ships its intro as assets/slideshow/<name>/frame_NN.png and packs it
during its asset build, which this port does not have. The container is small:
an 8-byte header, then each frame as a uint16 length followed by icon frame
data -- the same encoding png2icon.py already produces, so the PNG decoding is
reused from there rather than duplicated.

    python tools/png2slideshow.py <frames-dir> <output.bin>

Frames must be 1-bit greyscale non-interlaced PNGs, all the same size, named so
that sorting by filename gives playback order.
"""

import argparse
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from png2icon import png_to_icon  # noqa: E402

SLIDESHOW_MAGIC = 0x72676468
SLIDESHOW_VERSION = 1
SLIDESHOW_MAX_FRAMES = 255


def pack(directory):
    frames = sorted(pathlib.Path(directory).glob("frame_*.png"))
    if not frames:
        raise SystemExit(f"{directory}: no frame_*.png found")
    if len(frames) > SLIDESHOW_MAX_FRAMES:
        raise SystemExit(f"{directory}: {len(frames)} frames, limit is {SLIDESHOW_MAX_FRAMES}")

    blobs = []
    width = height = None
    for frame in frames:
        frame_width, frame_height, blob = png_to_icon(frame)
        if width is None:
            width, height = frame_width, frame_height
        elif (frame_width, frame_height) != (width, height):
            raise SystemExit(
                f"{frame}: {frame_width}x{frame_height} does not match "
                f"{width}x{height} from {frames[0].name}"
            )
        # The frame header length is a uint16, so an oversized frame would wrap
        # silently and the device would read garbage.
        if len(blob) > 0xFFFF:
            raise SystemExit(f"{frame}: encoded frame is {len(blob)} bytes, limit is 65535")
        blobs.append(blob)

    out = bytearray(
        struct.pack("<IBBBB", SLIDESHOW_MAGIC, SLIDESHOW_VERSION, width, height, len(blobs))
    )
    for blob in blobs:
        out += struct.pack("<H", len(blob))
        out += blob

    return width, height, len(blobs), bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", help="directory holding frame_NN.png")
    parser.add_argument("output", help="path of the .bin to write")
    args = parser.parse_args()

    width, height, count, blob = pack(args.directory)
    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(blob)

    print(f"{output}: {count} frames, {width}x{height}, {len(blob)} bytes")


if __name__ == "__main__":
    main()
