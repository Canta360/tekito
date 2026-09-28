#!/usr/bin/env python3
"""Turns the frames written by tekito_typing_demo into an animated PNG.

    python scripts/make-demo-animation.py <frames folder> docs/images/typing-demo.png [--trim N]

--trim N crops every frame to what differs from the white page in any frame,
plus N pixels around it (the demo records on a page as large as the screen).

Only the standard library is used. Each frame after the first stores just
the rectangle that changed, which keeps the file small.
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path


def read_bmp(path: Path) -> tuple[int, int, list[bytes]]:
    """Rows of RGB bytes from a 32-bit top-down BMP."""
    data = path.read_bytes()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    top_down = height < 0
    height = abs(height)
    rows = []
    for y in range(height):
        start = offset + y * width * 4
        bgra = data[start:start + width * 4]
        rgb = bytearray(width * 3)
        rgb[0::3] = bgra[2::4]
        rgb[1::3] = bgra[1::4]
        rgb[2::3] = bgra[0::4]
        rows.append(bytes(rgb))
    if not top_down:
        rows.reverse()
    return width, height, rows


def chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))


def compress(rows: list[bytes], left: int, right: int) -> bytes:
    raw = b"".join(b"\x00" + row[left * 3:right * 3] for row in rows)
    return zlib.compress(raw, 9)


def changed_box(previous: list[bytes], current: list[bytes], width: int) -> tuple[int, int, int, int]:
    changed_rows = [y for y, (a, b) in enumerate(zip(previous, current)) if a != b]
    if not changed_rows:
        return 0, 0, 1, 1
    top, bottom = changed_rows[0], changed_rows[-1] + 1
    left, right = width, 0
    for y in changed_rows:
        a, b = previous[y], current[y]
        first = next(x for x in range(width) if a[x * 3:x * 3 + 3] != b[x * 3:x * 3 + 3])
        last = next(x for x in range(width - 1, -1, -1) if a[x * 3:x * 3 + 3] != b[x * 3:x * 3 + 3])
        left, right = min(left, first), max(right, last + 1)
    return left, top, right, bottom


def drawn_box(frames: list, width: int, height: int) -> tuple[int, int, int, int]:
    """What differs from the white page in any frame."""
    left, top, right, bottom = width, height, 0, 0
    for (_, _, rows), _ in frames:
        for y, row in enumerate(rows):
            start = len(row) - len(row.lstrip(b"\xff"))
            if start == len(row):
                continue
            end = len(row.rstrip(b"\xff"))
            left, right = min(left, start // 3), max(right, (end + 2) // 3)
            top, bottom = min(top, y), max(bottom, y + 1)
    return left, top, right, bottom


def main() -> int:
    folder, output = Path(sys.argv[1]), Path(sys.argv[2])
    trim = int(sys.argv[sys.argv.index("--trim") + 1]) if "--trim" in sys.argv else None
    timings = [line.split() for line in (folder / "frames.txt").read_text().splitlines() if line.strip()]
    frames = [(read_bmp(folder / name), int(milliseconds)) for name, milliseconds in timings]
    (width, height, _), _ = frames[0]
    if trim is not None:
        left, top, right, bottom = drawn_box(frames, width, height)
        left, top = max(0, left - trim), max(0, top - trim)
        right, bottom = min(width, right + trim), min(height, bottom + trim)
        frames = [((right - left, bottom - top, [row[left * 3:right * 3] for row in rows[top:bottom]]), ms)
                  for (_, _, rows), ms in frames]
        width, height = right - left, bottom - top

    png = [b"\x89PNG\r\n\x1a\n",
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)),
           chunk(b"acTL", struct.pack(">II", len(frames), 0))]
    sequence = 0
    previous = None
    for index, ((_, _, rows), milliseconds) in enumerate(frames):
        if previous is None:
            left, top, right, bottom = 0, 0, width, height
        else:
            left, top, right, bottom = changed_box(previous, rows, width)
        png.append(chunk(b"fcTL", struct.pack(">IIIIIHHBB", sequence, right - left, bottom - top,
                                              left, top, milliseconds, 1000, 0, 0)))
        sequence += 1
        data = compress(rows[top:bottom], left, right)
        if index == 0:
            png.append(chunk(b"IDAT", data))
        else:
            png.append(chunk(b"fdAT", struct.pack(">I", sequence) + data))
            sequence += 1
        previous = rows
    png.append(chunk(b"IEND", b""))
    output.write_bytes(b"".join(png))
    print(f"{output}: {len(frames)} frames, {output.stat().st_size / 1024:.0f} KB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
