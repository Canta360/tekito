"""Builds the japanese-variants Data Pack: for each kanji, its variant forms
(異体字: 髙 for 高, 﨑 for 崎, 澤 for 沢, 國 for 国), offered after a
reading's words in kana-kanji conversion, mostly for names.

Sources, in the order a kanji's variants come:
  - Unicode's Unihan database (Unihan_Variants.txt, Unicode License V3):
    kJapaneseOldVariant and kJapaneseNewVariant (旧字体 and 新字体);
  - SKK-JISYO.itaiji (SKK dictionaries, public domain), as ordered there;
  - Unihan's kZVariant, kSemanticVariant and kSpoofingVariant (𠮷 for 吉).
Chinese simplified and traditional pairs are left out. Apart from 旧字体 and
新字体, a pair of two kanji in common use (常用 or 人名用 in the kanji-sets
pack: 崎 and 埼, 広 and 宏) or of two rare ones is left out: one of the two
must be the common kanji. Both must have a Japanese source in Unihan
(kIRG_JSource) or be in cp932, so Japanese fonts have them. Variants go
both ways (沢: 澤, 澤: 沢). A kanji's variants come 旧字体 or 新字体 first,
then those in cp932 (﨑, which Windows fonts all have), then the rest.

Usage:
  python scripts/ja/build-japanese-variants-pack.py --itaiji SKK-JISYO.itaiji --unihan <Unihan folder> \\
      --unicode-license <Unicode LICENSE> [--out data/ja/japanese-variants]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OLD_AND_NEW = ("kJapaneseOldVariant", "kJapaneseNewVariant")
OTHER_FIELDS = ("kZVariant", "kSemanticVariant", "kSpoofingVariant")
MOST_VARIANTS = 5


def is_kanji(c: str) -> bool:
    code = ord(c)
    return 0x3400 <= code <= 0x9FFF or 0xF900 <= code <= 0xFAFF or 0x20000 <= code <= 0x3FFFF


def in_cp932(c: str) -> bool:
    try:
        c.encode("cp932")
        return True
    except UnicodeEncodeError:
        return False


def japanese_sources(path: Path) -> set[str]:
    """The kanji with a Japanese source (kIRG_JSource) in Unihan_IRGSources.txt."""
    kanji = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) == 3 and fields[1] == "kIRG_JSource":
            kanji.add(chr(int(fields[0][2:], 16)))
    return kanji


def itaiji_pairs(path: Path) -> list[tuple[str, str]]:
    """(kanji, variant) from SKK-JISYO.itaiji (EUC-JIS-2004): 沢 /澤/."""
    pairs = []
    for line in path.read_bytes().decode("euc_jis_2004").splitlines():
        if line.startswith(";") or " /" not in line:
            continue
        key, values = line.split(" /", 1)
        for value in values.strip("/").split("/"):
            pairs.append((key, value.split(";", 1)[0]))
    return pairs


def unihan_pairs(path: Path, fields: tuple[str, ...]) -> list[tuple[str, str]]:
    """(kanji, variant) from Unihan_Variants.txt, field by field in the order given."""
    by_field: dict[str, list[tuple[str, str]]] = {field: [] for field in fields}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        code, field, values = line.split("\t")
        if field in by_field:
            kanji = chr(int(code[2:], 16))
            by_field[field] += [(kanji, chr(int(v, 16))) for v in re.findall(r"U\+([0-9A-F]+)", values)]
    return [pair for field in fields for pair in by_field[field]]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--itaiji", type=Path, required=True, help="SKK-JISYO.itaiji")
    parser.add_argument("--unihan", type=Path, required=True,
                        help="the Unihan folder (Unihan_Variants.txt, Unihan_IRGSources.txt)")
    parser.add_argument("--unicode-license", type=Path, required=True, help="Unicode License V3")
    parser.add_argument("--kanji-sets", type=Path, default=ROOT / "data" / "ja" / "kanji-sets" / "kanji.tsv")
    parser.add_argument("--out", type=Path, default=ROOT / "data" / "ja" / "japanese-variants")
    args = parser.parse_args()

    sources = japanese_sources(args.unihan / "Unihan_IRGSources.txt")
    common = {line.split("\t", 1)[0] for line in args.kanji_sets.read_text(encoding="utf-8").splitlines()}
    table = args.unihan / "Unihan_Variants.txt"
    pairs = ([(a, b, True) for a, b in unihan_pairs(table, OLD_AND_NEW)]
             + [(a, b, False) for a, b in itaiji_pairs(args.itaiji)]
             + [(a, b, False) for a, b in unihan_pairs(table, OTHER_FIELDS)])

    # A compatibility ideograph that text normalizes to another (隆 to 隆)
    # is no variant; those that stay (﨑) are.
    def japanese(c: str) -> bool:
        return is_kanji(c) and unicodedata.normalize("NFC", c) == c and (c in sources or in_cp932(c))

    variants: dict[str, list[tuple[int, str]]] = {}
    for kanji, variant, old_or_new in pairs:
        if len(kanji) != 1 or len(variant) != 1 or kanji == variant:
            continue
        if not (japanese(kanji) and japanese(variant)):
            continue
        if not old_or_new and (kanji in common) == (variant in common):
            continue
        for a, b in ((kanji, variant), (variant, kanji)):
            listed = variants.setdefault(a, [])
            if all(b != known for _, known in listed):
                listed.append((0 if old_or_new else 1 if in_cp932(b) else 2, b))

    args.out.mkdir(parents=True, exist_ok=True)
    data = args.out / "variants.tsv"
    with data.open("w", encoding="utf-8", newline="\n") as out:
        out.write("# kanji\tits variant forms (scripts/ja/build-japanese-variants-pack.py)\n")
        for kanji in sorted(variants):
            ordered = sorted(variants[kanji], key=lambda ranked: ranked[0])[:MOST_VARIANTS]
            out.write(f"{kanji}\t{''.join(b for _, b in ordered)}\n")
    (args.out / "NOTICE").write_text(
        "The kanji variant forms (異体字) come from Unicode's Unihan database\n"
        "(Unihan_Variants.txt and Unihan_IRGSources.txt, https://www.unicode.org/charts/unihan.html)\n"
        "and from SKK-JISYO.itaiji of the SKK dictionaries (https://github.com/skk-dev/dict),\n"
        "whose contents are in the public domain. Unihan is under the Unicode License V3:\n\n"
        + args.unicode_license.read_text(encoding="utf-8"),
        encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": "japanese-variants",
        "display_name": "Kanji Variant Forms",
        "schema_version": 1,
        "version": "2026.10",
        "language": "ja-JP",
        "type": "japanese-variants",
        "format": "tsv",
        "file": "variants.tsv",
        "entry_count": len(variants),
        "sha256": {"file": hashlib.sha256(data.read_bytes()).hexdigest().upper()},
        "license": "Unicode-3.0 (Unihan) + public domain (SKK-JISYO.itaiji)",
        "ai_training": True,
        "source": "https://www.unicode.org/Public/UCD/latest/ucd/Unihan.zip (Unihan_Variants.txt, "
                  "Unihan_IRGSources.txt), https://github.com/skk-dev/dict (SKK-JISYO.itaiji)",
        "notice_file": "NOTICE",
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                            encoding="utf-8", newline="\n")
    print(f"japanese-variants: {len(variants)} kanji")


if __name__ == "__main__":
    main()
