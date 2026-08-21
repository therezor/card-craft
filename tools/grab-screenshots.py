#!/usr/bin/env python3
"""Pull frames off a Cardputer running the cardputer-dev build and write PNGs.

    pio run -e cardputer-dev -t upload
    ./tools/grab-screenshots.py --out docs/img

Sends 's' and reads back one frame in the wire format screenshot.h documents.
Standard library only apart from pyserial, and the PNG encoder is right here,
so regenerating the README images needs nothing installed beyond the serial
module PlatformIO already ships.
"""

import argparse
import struct
import sys
import time
import zlib
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit(
        "pyserial not found. Either:\n"
        "  ~/.platformio/penv/bin/python -m pip install pyserial\n"
        "or run this with PlatformIO's interpreter:\n"
        "  ~/.platformio/penv/bin/python tools/grab-screenshots.py"
    )

MAGIC_IN, MAGIC_OUT = b"<<SHOT ", b">>SHOT"


def rgb565_to_rgb888(data, w, h, big_endian):
    """Expand a raw RGB565 frame to 8-bit RGB rows, replicating the top bits
    into the low ones so a full-white pixel comes out as 0xFF not 0xF8."""
    rows = []
    fmt = ">H" if big_endian else "<H"
    for y in range(h):
        row = bytearray()
        base = y * w * 2
        for x in range(w):
            (v,) = struct.unpack_from(fmt, data, base + x * 2)
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
        rows.append(bytes(row))
    return rows


def write_png(path, rows, w, h, scale=1):
    if scale > 1:
        scaled = []
        for row in rows:
            wide = b"".join(row[x * 3:x * 3 + 3] * scale for x in range(w))
            scaled.extend([wide] * scale)
        rows, w, h = scaled, w * scale, h * scale

    raw = b"".join(b"\x00" + row for row in rows)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


def read_until(ser, marker, limit):
    buf = bytearray()
    while marker not in buf:
        chunk = ser.read(1)
        if not chunk:
            return None
        buf += chunk
        if len(buf) > limit:
            return None
    return buf


def grab(ser, timeout):
    ser.reset_input_buffer()
    ser.write(b"s")
    ser.flush()

    header = read_until(ser, MAGIC_IN, 8192)
    if header is None:
        return None, None
    line = ser.readline().decode("ascii", "replace").strip()
    parts = line.split()
    if len(parts) < 4:
        print(f"  malformed header: {line!r}", file=sys.stderr)
        return None, None
    name, w, h, order = parts[0], int(parts[1]), int(parts[2]), parts[3]

    need = w * h * 2
    data = bytearray()
    deadline = time.time() + timeout
    while len(data) < need and time.time() < deadline:
        data += ser.read(need - len(data))
    if len(data) < need:
        print(f"  short frame: {len(data)}/{need} bytes", file=sys.stderr)
        return None, None

    read_until(ser, MAGIC_OUT, 64)
    return name, rgb565_to_rgb888(bytes(data), w, h, order == "be")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem201301")
    ap.add_argument("--out", default="docs/img", type=Path)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--count", type=int, default=1, help="frames to grab")
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between grabs")
    ap.add_argument("--timeout", type=float, default=10.0)
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    saved = 0
    with serial.Serial(args.port, 115200, timeout=1.0) as ser:
        time.sleep(0.4)
        for i in range(args.count):
            if i:
                time.sleep(args.interval)
            name, rows = grab(ser, args.timeout)
            if not rows:
                print(f"  grab {i + 1} failed", file=sys.stderr)
                continue
            h, w = len(rows), len(rows[0]) // 3
            stem = name if args.count == 1 else f"{name}-{i:02d}"
            path = args.out / f"{stem}.png"
            write_png(path, rows, w, h, args.scale)
            print(f"  {path}  {w}x{h} x{args.scale}")
            saved += 1
    return 0 if saved else 1


if __name__ == "__main__":
    sys.exit(main())
