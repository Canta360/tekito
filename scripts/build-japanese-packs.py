"""Builds the japanese-core data pack from the Mozc OSS dictionary.

The pack holds two memory-mappable files:

  dictionary.bin  (ja-dict-v1)   readings -> (left id, right id, cost, surface)
  connection.bin  (ja-matrix-v1) the cost of each right id followed by each left id

Both are little-endian and read in place: TEKITO maps them into memory and
never parses or copies them, so every process shares the same pages.

ja-dict-v1
  header       magic "TKJD", u32 version (1), then u32 fields, see HEADER
  reverse map  u8[65536]: UTF-16 code unit -> reading code (0 = not a reading character)
  char table   u16[char_count]: reading code - 1 -> UTF-16 code unit
  key offsets  u32[key_count + 1] into the key blob
  key blob     reading codes, keys sorted bytewise
  token starts u32[key_count + 1] into the token array
  tokens       10 bytes each: u16 left id, u16 right id, u16 cost, u32 surface
  surfaces     u16 units: [length][code units...]

  A token's surface field holds the kind in bits 30-31 (0 = string in the
  surface pool at the offset in bits 0-28, 1 = same as the reading,
  2 = the reading in katakana) and, in bit 29, whether Mozc lists the entry
  as a spelling correction (shown as a suggestion, never chosen on its own).

ja-matrix-v1
  header       magic "TKJM", u32 version (1), u32 size, u32 format, u32 scale
  costs        format 0: i16[size * size]; format 1: u8[size * size] times scale
               indexed [right id of the previous word * size + left id of the next]

Usage:
  python scripts/build-japanese-packs.py --mozc <dir with dictionary_oss/> --out data/japanese-core
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

HEADER = struct.Struct("<4sIIIIIIIIIIIII")  # magic, version, then the fields below
HEADER_FIELDS = (
    "key_count", "token_count", "char_count", "surface_units",
    "reverse_offset", "chars_offset", "key_offsets_offset", "key_blob_offset",
    "token_starts_offset", "tokens_offset", "surfaces_offset", "file_size",
)
TOKEN = struct.Struct("<HHHI")

SURFACE_POOL = 0
SURFACE_READING = 1
SURFACE_KATAKANA = 2
SPELLING_CORRECTION_BIT = 1 << 29


def katakana(text: str) -> str:
    return "".join(chr(ord(c) + 0x60) if "ぁ" <= c <= "ゖ" else c for c in text)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def read_mozc_entries(mozc: Path):
    entries = []
    for path in sorted((mozc / "dictionary_oss").glob("dictionary0*.txt")):
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                fields = line.rstrip("\n").split("\t")
                if len(fields) < 5:
                    raise ValueError(f"{path.name}: expected 5 columns: {line!r}")
                reading, left, right, cost, surface = fields[:5]
                spelling = len(fields) > 5 and fields[5] == "SPELLING_CORRECTION"
                entries.append((reading, int(left), int(right), int(cost), surface, spelling))
    return entries


def align(blob: bytearray, boundary: int) -> None:
    blob.extend(b"\0" * (-len(blob) % boundary))


def build_dictionary(entries, out: Path) -> dict:
    chars = sorted({c for reading, *_ in entries for c in reading})
    if len(chars) > 255:
        raise ValueError(f"{len(chars)} reading characters; ja-dict-v1 codes hold 255")
    if any(ord(c) > 0xFFFF for c in chars):
        raise ValueError("reading characters must be in the BMP")
    code_of = {c: i + 1 for i, c in enumerate(chars)}

    def encode(reading: str) -> bytes:
        return bytes(code_of[c] for c in reading)

    by_key: dict[bytes, list] = {}
    for entry in entries:
        by_key.setdefault(encode(entry[0]), []).append(entry)
    keys = sorted(by_key)

    surface_units: list[int] = []
    surface_offset: dict[str, int] = {}
    key_offsets, key_blob, token_starts, tokens = [0], bytearray(), [0], bytearray()
    for key in keys:
        key_blob.extend(key)
        key_offsets.append(len(key_blob))
        # Cheapest first, so a reader that stops early keeps the likely words.
        for reading, left, right, cost, surface, spelling in sorted(by_key[key], key=lambda e: e[3]):
            if surface == reading:
                ref = SURFACE_READING << 30
            elif surface == katakana(reading):
                ref = SURFACE_KATAKANA << 30
            else:
                offset = surface_offset.get(surface)
                if offset is None:
                    units = surface.encode("utf-16-le")
                    offset = len(surface_units)
                    surface_offset[surface] = offset
                    surface_units.append(len(units) // 2)
                    surface_units.extend(struct.unpack(f"<{len(units) // 2}H", units))
                if offset >= SPELLING_CORRECTION_BIT:
                    raise ValueError("surface pool too large for ja-dict-v1")
                ref = (SURFACE_POOL << 30) | offset
            if spelling:
                ref |= SPELLING_CORRECTION_BIT
            tokens.extend(TOKEN.pack(left, right, cost, ref))
        token_starts.append(len(tokens) // TOKEN.size)

    reverse = bytearray(65536)
    for c, code in code_of.items():
        reverse[ord(c)] = code

    blob = bytearray(HEADER.size)
    offsets = {}
    for name, data, boundary in (
        ("reverse_offset", bytes(reverse), 4),
        ("chars_offset", struct.pack(f"<{len(chars)}H", *map(ord, chars)), 4),
        ("key_offsets_offset", struct.pack(f"<{len(key_offsets)}I", *key_offsets), 4),
        ("key_blob_offset", bytes(key_blob), 4),
        ("token_starts_offset", struct.pack(f"<{len(token_starts)}I", *token_starts), 4),
        ("tokens_offset", bytes(tokens), 4),
        ("surfaces_offset", struct.pack(f"<{len(surface_units)}H", *surface_units), 4),
    ):
        align(blob, boundary)
        offsets[name] = len(blob)
        blob.extend(data)
    fields = dict(offsets, key_count=len(keys), token_count=len(tokens) // TOKEN.size,
                  char_count=len(chars), surface_units=len(surface_units), file_size=len(blob))
    blob[: HEADER.size] = HEADER.pack(b"TKJD", 1, *(fields[name] for name in HEADER_FIELDS))
    out.write_bytes(blob)
    sections = {
        "key_blob": len(key_blob),
        "key_offsets": 4 * len(key_offsets),
        "token_starts": 4 * len(token_starts),
        "tokens": len(tokens),
        "surfaces": 2 * len(surface_units),
        "reverse_map": len(reverse),
    }
    return {"keys": len(keys), "tokens": fields["token_count"], "chars": len(chars),
            "pooled_surfaces": len(surface_offset), "bytes": len(blob), "sections": sections}


def build_matrix(mozc: Path, out: Path, quantize: int) -> dict:
    values = (mozc / "dictionary_oss" / "connection_single_column.txt").read_text().split()
    size = int(values[0])
    costs = list(map(int, values[1:]))
    if len(costs) != size * size:
        raise ValueError(f"expected {size * size} connection costs, found {len(costs)}")
    if quantize:
        packed = bytes(min(255, (c + quantize // 2) // quantize) for c in costs)
        body, fmt = packed, 1
    else:
        if max(costs) > 32767:
            raise ValueError("connection cost does not fit in i16")
        body, fmt = struct.pack(f"<{len(costs)}h", *costs), 0
    out.write_bytes(struct.pack("<4sIIII", b"TKJM", 1, size, fmt, quantize or 1) + body)
    return {"size": size, "format": fmt, "scale": quantize or 1, "bytes": out.stat().st_size,
            "max_cost": max(costs)}


def write_notice(mozc: Path, out: Path, commit: str) -> None:
    readme = (mozc / "dictionary_oss" / "README.txt").read_text(encoding="utf-8")
    license_text = (mozc / "LICENSE").read_text(encoding="utf-8")
    out.write_text(
        "japanese-core is built from the Mozc OSS dictionary "
        f"(https://github.com/google/mozc, commit {commit}).\n\n"
        "== Mozc ==\n\n" + license_text.strip() + "\n\n"
        "== Mozc dictionary_oss (IPAdic, Okinawa dictionary) ==\n\n" + readme.strip() + "\n",
        encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--mozc", type=Path, required=True,
                        help="Mozc src/data directory holding dictionary_oss/ and LICENSE")
    parser.add_argument("--commit", required=True, help="Mozc commit the files came from")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--quantize", type=int, default=0,
                        help="store connection costs as u8 in steps of this size (0 = i16)")
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    dictionary = build_dictionary(read_mozc_entries(args.mozc), args.out / "dictionary.bin")
    matrix = build_matrix(args.mozc, args.out / "connection.bin", args.quantize)
    write_notice(args.mozc, args.out / "NOTICE", args.commit)
    manifest = {
        "pack_id": "japanese-core",
        "display_name": "Japanese Conversion Dictionary",
        "schema_version": 1,
        "version": "prototype",
        "language": "ja-JP",
        "type": "japanese-dictionary",
        "format": "ja-dict-v1+ja-matrix-v1",
        "file": "dictionary.bin",
        "index_file": "connection.bin",
        "entry_count": dictionary["tokens"],
        "sha256": {"file": sha256(args.out / "dictionary.bin"),
                   "index": sha256(args.out / "connection.bin")},
        "license": "IPAdic + BSD-3-Clause (Mozc)",
        "source": f"https://github.com/google/mozc/tree/{args.commit}/src/data/dictionary_oss",
        "notice_file": "NOTICE",
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                           encoding="utf-8", newline="\n")
    print(json.dumps({"dictionary": dictionary, "matrix": matrix}, indent=2))


if __name__ == "__main__":
    main()
