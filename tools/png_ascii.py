#!/usr/bin/env python3
"""Inspect a PNG written by the cycle host (cyc_png.c) and print it as ASCII.

The host writes 8-bit RGBA, non-interlaced PNGs.  This decodes them with the
standard library only, so no image package is needed on the build machine.

    python tools/png_ascii.py shot.png [--cols 64]
"""
import argparse
import struct
import sys
import zlib

RAMP = " .:-=+*#%@"


def decode_png(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("not a PNG")
    pos = 8
    idat = bytearray()
    width = height = depth = color = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if depth != 8:
        raise SystemExit(f"unsupported bit depth {depth}")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        filt = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            x = line[i]
            if filt == 1:
                x = (x + a) & 0xFF
            elif filt == 2:
                x = (x + b) & 0xFF
            elif filt == 3:
                x = (x + ((a + b) >> 1)) & 0xFF
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                x = (x + pr) & 0xFF
            line[i] = x
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, channels, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png")
    ap.add_argument("--cols", type=int, default=64)
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()
    with open(args.png, "rb") as handle:
        data = handle.read()
    width, height, channels, pixels = decode_png(data)
    if args.stats:
        colours = {}
        for i in range(0, len(pixels), channels):
            key = tuple(pixels[i:i + 3])
            colours[key] = colours.get(key, 0) + 1
        print(f"{args.png}: {width}x{height} channels={channels} unique_colours={len(colours)}")
        for key, count in sorted(colours.items(), key=lambda kv: -kv[1])[:12]:
            print(f"  #{key[0]:02X}{key[1]:02X}{key[2]:02X}  {count}")
    cols = max(8, args.cols)
    rows = max(1, int(round(height * cols / width / 2.1)))
    print(f"+{'-' * cols}+")
    for ry in range(rows):
        line = []
        for rx in range(cols):
            x0 = rx * width // cols
            x1 = max(x0 + 1, (rx + 1) * width // cols)
            y0 = ry * height // rows
            y1 = max(y0 + 1, (ry + 1) * height // rows)
            total = 0
            count = 0
            for y in range(y0, y1, 2):
                base = y * width * channels
                for x in range(x0, x1):
                    i = base + x * channels
                    total += (pixels[i] * 299 + pixels[i + 1] * 587 + pixels[i + 2] * 114) // 1000
                    count += 1
            lum = total // max(1, count)
            line.append(RAMP[min(len(RAMP) - 1, lum * len(RAMP) // 256)])
        print("|" + "".join(line) + "|")
    print("+" + "-" * cols + "+")


if __name__ == "__main__":
    main()
