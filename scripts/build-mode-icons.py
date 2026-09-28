#!/usr/bin/env python3
"""Packs the input mode icons (assets/icons/*.ico) from PNGs of their SVG
sources.

Each SVG in assets/icons (direct-dark, japanese, japanese-dark) is exported
at every icon size in SIZES as <name>-<size>.png, square with transparent
margins and its proportions kept (from the design tool, or by drawing the
SVG on a canvas in a browser). This packs each name's PNGs into
assets/icons/<name>.ico, so Windows picks the size it needs and the mode
indicator stays sharp at any scaling.

  python scripts/build-mode-icons.py --pngs <folder of PNGs>
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ICONS = ROOT / "assets" / "icons"
SOURCES = ["direct-dark", "japanese", "japanese-dark"]
SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def png_size(png: bytes) -> tuple[int, int]:
    if not png.startswith(PNG_SIGNATURE):
        raise ValueError("not a PNG")
    return struct.unpack(">II", png[16:24])


def write_ico(path: Path, images: list[tuple[int, bytes]]) -> None:
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries = b""
    data = b""
    for size, png in images:
        # Width and height 0 mean 256.
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(png), offset + len(data))
        data += png
    path.write_bytes(header + entries + data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--pngs", type=Path, required=True, help="folder of <name>-<size>.png")
    args = parser.parse_args()
    for name in SOURCES:
        images = []
        for size in SIZES:
            png = (args.pngs / f"{name}-{size}.png").read_bytes()
            if png_size(png) != (size, size):
                raise SystemExit(f"{name}-{size}.png is {png_size(png)}, not {size}x{size}")
            images.append((size, png))
        write_ico(ICONS / f"{name}.ico", images)
        print(f"wrote {ICONS / (name + '.ico')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
