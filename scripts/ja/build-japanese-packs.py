"""Builds the japanese-core data pack from the Mozc OSS dictionary.

The pack holds two memory-mappable files, little-endian, read in place:
TEKITO maps them and never parses or copies them, so every process shares
the same pages.

dictionary.bin (ja-dict-v1): readings -> words
  header       see DICT_HEADER; offsets are from the start of the file;
               unknown_id is the id of UNKNOWN_POS
  reverse map  u8[65536]: UTF-16 code unit -> reading code (0 = none)
  char table   u16[char_count]: reading code - 1 -> UTF-16 code unit
  record index u32[key_count + 1]: offsets of the records (the last one is
               the end), in bytewise order of their keys
  records      u8 key length, the key's reading codes, u16 word count, words
  surfaces     u16 units: [length][code units...]

  A word is: u8 flags, u16 left id, [u16 right id], u16 cost, [u24 surface]
    flags bits 0-1  surface: 0 = from the surface pool (the u24 offset, in
                    u16 units), 1 = the reading, 2 = the reading in katakana
    flags bit 2     Mozc lists it as a spelling correction: offer it, never
                    choose it on its own
    flags bit 3     the right id equals the left id and is not stored
  Words of a reading are sorted cheapest first.

connection.bin (ja-matrix-v1): how words join
  header       see MATRIX_HEADER
  costs        u8[size * size] times `scale`, indexed
               [right id of the previous word * size + left id of the next]
  row classes  u16[size]: the boundary class of each right id
  boundaries   class_count rows of ceil(size / 8) bytes: bit `left id` is set
               when a new phrase (bunsetsu) starts between the two words
               (Mozc's segmenter.def rules; id 0 is the start and end)

pos.tsv: the part of speech a word the user adds takes, by the kind Settings
offers (JapaneseUserDictionary.h): kind, left id, right id, cost; and, as
kind "number", the part of speech and cost of digits typed in Japanese, so
counters join them (3こ -> 3個). Comment lines start with #.

Usage:
  python scripts/ja/build-japanese-packs.py --mozc <Mozc src/data> --commit <sha> --out data/ja/japanese-core
  python scripts/ja/build-japanese-packs.py --mozc ... --commit ... --out tests/data/ja/japanese-mini \\
      --sentences tests/data/ja/japanese-mini-sentences.txt
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

DICT_HEADER = struct.Struct("<4s12I12x")  # magic + 12 fields + reserved
DICT_FIELDS = ("version", "key_count", "word_count", "char_count", "reverse_offset",
               "chars_offset", "index_offset", "records_offset", "surfaces_offset",
               "surface_units", "file_size", "unknown_id")
# The part of speech given to a character no word covers.
UNKNOWN_POS = "名詞,一般,*,*,*,*,*"
MATRIX_HEADER = struct.Struct("<4s9I")
MATRIX_FIELDS = ("version", "size", "format", "scale", "class_count", "costs_offset",
                 "classes_offset", "boundaries_offset", "file_size")

ROOT = Path(__file__).resolve().parents[2]

SURFACE_POOL, SURFACE_READING, SURFACE_KATAKANA = 0, 1, 2
FLAG_SPELLING_CORRECTION = 1 << 2
FLAG_SAME_IDS = 1 << 3
COST_SCALE = 64

# The kinds of words users add, and the Mozc part of speech of each.
USER_POS = (
    ("noun", "名詞,一般,*,*,*,*,*"),
    ("proper-noun", "名詞,固有名詞,一般,*,*,*,*"),
    ("person", "名詞,固有名詞,人名,一般,*,*,*"),
    ("surname", "名詞,固有名詞,人名,姓,*,*,*"),
    ("given-name", "名詞,固有名詞,人名,名,*,*,*"),
    ("place", "名詞,固有名詞,地域,一般,*,*,*"),
    ("organization", "名詞,固有名詞,組織,*,*,*,*"),
    ("suru-noun", "名詞,サ変接続,*,*,*,*,*"),
    ("symbol", "記号,一般,*,*,*,*,*"),
    ("interjection", "感動詞,*,*,*,*,*,*"),
)
# Cheaper than 95 % of Mozc's words: the user's words win their readings.
USER_WORD_COST = 4000
# Digits typed in Japanese read as one number, about as likely as Mozc's
# single digits (1904-3380).
NUMBER_POS = "名詞,数,アラビア数字,*,*,*,*"
NUMBER_COST = 2500


def katakana(text: str) -> str:
    return "".join(chr(ord(c) + 0x60) if "ぁ" <= c <= "ゖ" else c for c in text)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def align(blob: bytearray, boundary: int = 4) -> None:
    blob.extend(b"\0" * (-len(blob) % boundary))


def read_entries(mozc: Path):
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


def read_slang_entries(pos_names: list[str], base_entries):
    path = Path(__file__).with_name("japanese-slang-seeds.tsv")
    noun_id = pos_names.index(USER_POS[0][1])
    additions = []
    wanted = set()
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3 or not all(fields):
            raise ValueError(f"{path.name}:{number}: expected reading, surface and meaning")
        reading, surface, _meaning = fields
        if not re.fullmatch(r"[ぁ-ゟー]+", reading):
            raise ValueError(f"{path.name}:{number}: reading must be hiragana")
        key = (reading, surface)
        if key not in wanted:
            additions.append((reading, noun_id, noun_id, USER_WORD_COST, surface, False))
            wanted.add(key)
    existing = set()
    for entry in base_entries:
        key = (entry[0], entry[4])
        if key in wanted:
            existing.add(key)
    return [entry for entry in additions if (entry[0], entry[4]) not in existing]


def read_pos_names(mozc: Path) -> list[str]:
    names = []
    for line in (mozc / "dictionary_oss" / "id.def").read_text(encoding="utf-8").splitlines():
        number, name = line.split(" ", 1)
        if int(number) != len(names):
            raise ValueError("id.def is not numbered in order")
        names.append(name)
    return names


def read_matrix(mozc: Path) -> tuple[int, list[int]]:
    values = (mozc / "dictionary_oss" / "connection_single_column.txt").read_text().split()
    size = int(values[0])
    costs = list(map(int, values[1:]))
    if len(costs) != size * size:
        raise ValueError(f"expected {size * size} connection costs, found {len(costs)}")
    return size, costs


def boundary_rows(mozc: Path, pos_names: list[str]) -> list[int]:
    """Mozc's segmenter.def as one bitmask of left ids per right id."""
    size = len(pos_names)
    everything = (1 << size) - 1

    def ids(pattern: str) -> tuple[list[int], int]:
        if pattern == "*":
            return list(range(size)), everything
        regex = re.compile(pattern.replace("*", "[^,]+"))
        matched = [i for i, name in enumerate(pos_names) if regex.match(name)]
        return matched, sum(1 << i for i in matched)

    decided = [0] * size
    boundary = [0] * size
    rules = (mozc / "rules" / "segmenter.def").read_text(encoding="utf-8").splitlines()
    for line in rules:
        if not line.strip() or line.startswith("#"):
            continue
        left, right, result = line.split()
        rights, _ = ids(left)       # the previous word's right id
        _, lefts = ids(right)       # the next word's left id
        for r in rights:
            fresh = lefts & ~decided[r]
            if not fresh:
                continue
            decided[r] |= fresh
            if result.lower() == "true":
                boundary[r] |= fresh
    # Unmatched pairs, and anything next to the start or end, are boundaries.
    for r in range(size):
        boundary[r] |= everything & ~decided[r]
        boundary[r] |= 1  # before the end (left id 0)
    boundary[0] = everything  # after the start (right id 0)
    return boundary


def build_dictionary(entries, unknown_id: int, out: Path) -> dict:
    chars = sorted({c for reading, *_ in entries for c in reading})
    if len(chars) > 255:
        raise ValueError(f"{len(chars)} reading characters; ja-dict-v1 codes hold 255")
    if any(ord(c) > 0xFFFF for c in chars):
        raise ValueError("reading characters must be in the BMP")
    code_of = {c: i + 1 for i, c in enumerate(chars)}

    by_key: dict[bytes, list] = {}
    for entry in entries:
        by_key.setdefault(bytes(code_of[c] for c in entry[0]), []).append(entry)
    keys = sorted(by_key)

    surface_units: list[int] = []
    surface_offset: dict[str, int] = {}
    records = bytearray()
    index = []
    for key in keys:
        if len(key) > 255:
            raise ValueError("reading longer than 255 characters")
        words = sorted(by_key[key], key=lambda e: (e[3], e[4]))
        if len(words) > 0xFFFF:
            raise ValueError("too many words for one reading")
        index.append(len(records))
        records.append(len(key))
        records.extend(key)
        records.extend(struct.pack("<H", len(words)))
        for reading, left, right, cost, surface, spelling in words:
            if not (0 <= cost <= 0xFFFF and 0 <= left <= 0xFFFF and 0 <= right <= 0xFFFF):
                raise ValueError(f"value out of range for {reading}")
            if surface == reading:
                kind, offset = SURFACE_READING, None
            elif surface == katakana(reading):
                kind, offset = SURFACE_KATAKANA, None
            else:
                kind = SURFACE_POOL
                offset = surface_offset.get(surface)
                if offset is None:
                    units = surface.encode("utf-16-le")
                    offset = len(surface_units)
                    surface_offset[surface] = offset
                    surface_units.append(len(units) // 2)
                    surface_units.extend(struct.unpack(f"<{len(units) // 2}H", units))
                if offset >= 1 << 24:
                    raise ValueError("surface pool too large for ja-dict-v1")
            flags = kind | (FLAG_SPELLING_CORRECTION if spelling else 0)
            if left == right:
                flags |= FLAG_SAME_IDS
            records.append(flags)
            records.extend(struct.pack("<H", left))
            if left != right:
                records.extend(struct.pack("<H", right))
            records.extend(struct.pack("<H", cost))
            if offset is not None:
                records.extend(offset.to_bytes(3, "little"))
    index.append(len(records))

    reverse = bytearray(65536)
    for c, code in code_of.items():
        reverse[ord(c)] = code

    blob = bytearray(DICT_HEADER.size)
    offsets = {}
    for name, data in (
        ("reverse_offset", bytes(reverse)),
        ("chars_offset", struct.pack(f"<{len(chars)}H", *map(ord, chars))),
        ("index_offset", b""),  # placeholder, filled below
        ("records_offset", bytes(records)),
        ("surfaces_offset", struct.pack(f"<{len(surface_units)}H", *surface_units)),
    ):
        align(blob)
        offsets[name] = len(blob)
        if name == "index_offset":
            blob.extend(struct.pack(f"<{len(index)}I", *index))
        else:
            blob.extend(data)
    fields = dict(offsets, version=1, key_count=len(keys), word_count=len(entries),
                  char_count=len(chars), surface_units=len(surface_units), file_size=len(blob),
                  unknown_id=unknown_id)
    blob[: DICT_HEADER.size] = DICT_HEADER.pack(b"TKJD", *(fields[name] for name in DICT_FIELDS))
    out.write_bytes(blob)
    return {"keys": len(keys), "words": len(entries), "chars": len(chars),
            "pooled_surfaces": len(surface_offset), "bytes": len(blob),
            "sections": {"records": len(records), "index": 4 * len(index),
                         "surfaces": 2 * len(surface_units), "reverse_map": len(reverse)}}


def build_matrix(size: int, costs: list[int], boundary: list[int], out: Path) -> dict:
    row_bytes = (size + 7) // 8
    classes: dict[int, int] = {}
    row_class = []
    for mask in boundary:
        row_class.append(classes.setdefault(mask, len(classes)))
    blob = bytearray(MATRIX_HEADER.size)
    offsets = {}
    align(blob)
    offsets["costs_offset"] = len(blob)
    blob.extend(min(255, (c + COST_SCALE // 2) // COST_SCALE) for c in costs)
    align(blob)
    offsets["classes_offset"] = len(blob)
    blob.extend(struct.pack(f"<{size}H", *row_class))
    align(blob)
    offsets["boundaries_offset"] = len(blob)
    for mask in classes:
        blob.extend(mask.to_bytes(row_bytes, "little"))
    fields = dict(offsets, version=1, size=size, format=1, scale=COST_SCALE,
                  class_count=len(classes), file_size=len(blob))
    blob[: MATRIX_HEADER.size] = MATRIX_HEADER.pack(b"TKJM", *(fields[n] for n in MATRIX_FIELDS))
    out.write_bytes(blob)
    return {"size": size, "boundary_classes": len(classes), "bytes": len(blob),
            "max_cost": max(costs)}


def keep_for_sentences(entries, sentences: list[str], unknown_id: int, keep_ids: list[int]):
    """Only the words whose reading occurs in the given readings, with the
    part-of-speech ids they use (and `keep_ids`) renumbered from 1 (0 stays
    the start/end)."""
    wanted = [e for e in entries if any(e[0] in s for s in sentences)]
    used = sorted({0, unknown_id} | set(keep_ids) | {e[1] for e in wanted} | {e[2] for e in wanted})
    remap = {old: new for new, old in enumerate(used)}
    return [(r, remap[l], remap[rt], c, s, sp) for r, l, rt, c, s, sp in wanted], used


def write_user_pos(ids: list[int], number_id: int, out: Path) -> None:
    lines = ["# kind\tleft id\tright id\tcost (scripts/ja/build-japanese-packs.py)"]
    lines += [f"{kind}\t{pos}\t{pos}\t{USER_WORD_COST}" for (kind, _), pos in zip(USER_POS, ids)]
    lines.append(f"number\t{number_id}\t{number_id}\t{NUMBER_COST}")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def write_notice(mozc: Path, out: Path, commit: str) -> None:
    readme = (mozc / "dictionary_oss" / "README.txt").read_text(encoding="utf-8")
    license_text = (mozc / "LICENSE").read_text(encoding="utf-8")
    out.write_text(
        "japanese-core is built from the Mozc OSS dictionary "
        f"(https://github.com/google/mozc, commit {commit}).\n\n"
        "== Mozc ==\n\n" + license_text.strip() + "\n\n"
        "== Mozc dictionary_oss (IPAdic, Okinawa dictionary) ==\n\n" + readme.strip() + "\n\n"
        "== TEKITO Japanese slang additions ==\n\n"
        "The curated entries and original short definitions come from "
        "scripts/ja/japanese-slang-seeds.tsv and are covered by the TEKITO source license.\n",
        encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--mozc", type=Path, required=True,
                        help="Mozc src/data directory holding dictionary_oss/, rules/ and LICENSE")
    parser.add_argument("--commit", required=True, help="Mozc commit the files came from")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--sentences", type=Path,
                        help="build a small pack for tests: keep only words found in these readings")
    args = parser.parse_args()

    entries = read_entries(args.mozc)
    pos_names = read_pos_names(args.mozc)
    entries.extend(read_slang_entries(pos_names, entries))
    size, costs = read_matrix(args.mozc)
    if size != len(pos_names):
        raise ValueError("id.def and the connection matrix disagree on the id count")
    unknown_id = pos_names.index(UNKNOWN_POS)
    user_ids = [pos_names.index(name) for _, name in USER_POS]
    number_id = pos_names.index(NUMBER_POS)
    if args.sentences:
        sentences = [line.strip() for line in args.sentences.read_text(encoding="utf-8").splitlines()
                     if line.strip() and not line.startswith("#")]
        entries, used = keep_for_sentences(entries, sentences, unknown_id, user_ids + [number_id])
        costs = [costs[r * size + l] for r in used for l in used]
        pos_names = [pos_names[i] for i in used]
        unknown_id = used.index(unknown_id)
        user_ids = [used.index(i) for i in user_ids]
        number_id = used.index(number_id)
        size = len(used)
    boundary = boundary_rows(args.mozc, pos_names)

    args.out.mkdir(parents=True, exist_ok=True)
    dictionary = build_dictionary(entries, unknown_id, args.out / "dictionary.bin")
    matrix = build_matrix(size, costs, boundary, args.out / "connection.bin")
    write_user_pos(user_ids, number_id, args.out / "pos.tsv")
    write_notice(args.mozc, args.out / "NOTICE", args.commit)
    manifest = {
        "pack_id": "japanese-core",
        "display_name": "Japanese Conversion Dictionary",
        "schema_version": 1,
        "version": f"mozc-{args.commit[:12]}-slang-2026.09",
        "language": "ja-JP",
        "type": "japanese-dictionary",
        "format": "ja-dict-v1+ja-matrix-v1",
        "file": "dictionary.bin",
        "index_file": "connection.bin",
        "pos_file": "pos.tsv",
        "entry_count": dictionary["words"],
        "sha256": {"file": sha256(args.out / "dictionary.bin"),
                   "index": sha256(args.out / "connection.bin"),
                   "pos": sha256(args.out / "pos.tsv")},
        "license": "IPAdic + BSD-3-Clause (Mozc), TEKITO-OWNED",
        "source": f"https://github.com/google/mozc/tree/{args.commit}/src/data",
        "supplemental_source": "scripts/ja/japanese-slang-seeds.tsv",
        "notice_file": "NOTICE",
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                           encoding="utf-8", newline="\n")
    print(json.dumps({"dictionary": dictionary, "matrix": matrix}, indent=2))


if __name__ == "__main__":
    main()
