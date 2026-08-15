#!/usr/bin/env python3
"""Convert Flipper icon PNGs into the C arrays used by components/assets/assets_icons.c.

This port ships assets_icons.c pre-generated and has no asset build step, so
icons taken from upstream have to be encoded by hand. Icons are emitted
uncompressed: a single 0x00 header byte followed by XBM-style rows, which is
what compress_icon_decode() falls back to when the header says "not
compressed".

Only what Flipper icons actually use is supported: 1-bit greyscale,
non-interlaced PNG. Anything else is rejected rather than silently
mis-encoded.

Usage:
    python tools/png2icon.py NAME PATH.png [NAME2 PATH2.png ...]
    python tools/png2icon.py --self-test
"""

import argparse
import pathlib
import struct
import sys
import zlib

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def _read_chunks(data):
    if data[:8] != PNG_MAGIC:
        raise ValueError("not a PNG")
    offset = 8
    while offset < len(data):
        (length,) = struct.unpack(">I", data[offset : offset + 4])
        kind = data[offset + 4 : offset + 8]
        payload = data[offset + 8 : offset + 8 + length]
        yield kind, payload
        offset += 12 + length


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _unfilter(raw, height, row_bytes):
    """Undo PNG scanline filtering. bpp is 1 for the 1-bit greyscale we accept,
    since filters operate on bytes and sub-byte pixels round up to one byte."""
    out = bytearray()
    previous = bytearray(row_bytes)
    pos = 0
    for _ in range(height):
        filter_type = raw[pos]
        pos += 1
        line = bytearray(raw[pos : pos + row_bytes])
        pos += row_bytes
        if filter_type == 0:
            pass
        elif filter_type == 1:
            for i in range(1, row_bytes):
                line[i] = (line[i] + line[i - 1]) & 0xFF
        elif filter_type == 2:
            for i in range(row_bytes):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_type == 3:
            for i in range(row_bytes):
                left = line[i - 1] if i else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(row_bytes):
                left = line[i - 1] if i else 0
                upper_left = previous[i - 1] if i else 0
                line[i] = (line[i] + _paeth(left, previous[i], upper_left)) & 0xFF
        else:
            raise ValueError(f"unsupported PNG filter {filter_type}")
        out += line
        previous = line
    return bytes(out)


def png_to_icon(path):
    """Return (width, height, icon_bytes) with icon_bytes ready for assets_icons.c."""
    data = pathlib.Path(path).read_bytes()

    header = None
    idat = b""
    for kind, payload in _read_chunks(data):
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", payload[:13])
        elif kind == b"IDAT":
            idat += payload
        elif kind == b"IEND":
            break

    if header is None:
        raise ValueError("PNG has no IHDR")
    width, height, bit_depth, colour_type, _, _, interlace = header
    if (bit_depth, colour_type, interlace) != (1, 0, 0):
        raise ValueError(
            f"{path}: expected 1-bit greyscale non-interlaced, "
            f"got depth={bit_depth} colour_type={colour_type} interlace={interlace}"
        )

    row_bytes = (width + 7) // 8
    pixels = _unfilter(zlib.decompress(idat), height, row_bytes)

    # PNG greyscale: 0 is black, and the leftmost pixel is the most significant
    # bit. Flipper icons set a bit for an inked pixel and put the leftmost pixel
    # in the least significant bit, so each byte is inverted and bit-reversed.
    # Bits past the image width are padding and must be left clear, otherwise
    # inverting turns them into a stripe of set pixels down the icon's edge.
    tail_bits = width % 8
    tail_mask = (1 << tail_bits) - 1 if tail_bits else 0xFF

    out = bytearray([0x00])
    for row in range(height):
        row_start = row * row_bytes
        for column in range(row_bytes):
            inverted = (~pixels[row_start + column]) & 0xFF
            reversed_bits = int(f"{inverted:08b}"[::-1], 2)
            if column == row_bytes - 1:
                reversed_bits &= tail_mask
            out.append(reversed_bits)
    return width, height, bytes(out)


def format_icon(name, width, height, blob):
    body = ",".join(f"0x{b:02x}" for b in blob)
    return (
        f"const uint8_t _I_{name}_0[] = {{{body},}};\n"
        f"const uint8_t* const _I_{name}[] = {{_I_{name}_0}};\n"
        f"const Icon I_{name} = {{.width={width},.height={height},"
        f".frame_count=1,.frame_rate=0,.frames=_I_{name}}};\n"
    )


# Encoded by the upstream asset pipeline and already present in
# components/assets/assets_icons.c; if this converter agrees with it, the bit
# order and polarity above are right.
SELF_TEST_NAME = "Pin_back_arrow_10x8"
SELF_TEST_RELATIVE = "assets/icons/PIN/Pin_back_arrow_10x8.png"
SELF_TEST_EXPECTED = bytes(
    [
        0x00, 0x04, 0x00, 0x06, 0x00, 0xFF, 0x00, 0x06, 0x01,
        0x04, 0x02, 0x00, 0x02, 0x00, 0x01, 0xF8, 0x00,
    ]
)


def self_test(momentum_root):
    png = pathlib.Path(momentum_root) / SELF_TEST_RELATIVE
    if not png.is_file():
        print(f"self-test: missing {png}", file=sys.stderr)
        return 1
    width, height, blob = png_to_icon(png)
    if (width, height) != (10, 8):
        print(f"self-test: got {width}x{height}, expected 10x8", file=sys.stderr)
        return 1
    if blob != SELF_TEST_EXPECTED:
        print("self-test: encoding mismatch", file=sys.stderr)
        print("  expected " + ",".join(f"0x{b:02x}" for b in SELF_TEST_EXPECTED), file=sys.stderr)
        print("  got      " + ",".join(f"0x{b:02x}" for b in blob), file=sys.stderr)
        return 1
    print(f"self-test passed: {SELF_TEST_NAME} matches assets_icons.c")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pairs", nargs="*", metavar="NAME PATH")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="check the encoder against an icon already in assets_icons.c",
    )
    parser.add_argument(
        "--momentum-root",
        default=str(pathlib.Path(__file__).resolve().parents[2] / "Momentum-Firmware"),
        help="upstream checkout used by --self-test",
    )
    args = parser.parse_args()

    if args.self_test:
        return self_test(args.momentum_root)

    if not args.pairs or len(args.pairs) % 2:
        parser.error("expected NAME PATH pairs")

    for i in range(0, len(args.pairs), 2):
        name, path = args.pairs[i], args.pairs[i + 1]
        width, height, blob = png_to_icon(path)
        sys.stdout.write(format_icon(name, width, height, blob))
    return 0


if __name__ == "__main__":
    sys.exit(main())
