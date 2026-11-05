"""The japanese-notation pack: dictionary words written in kanji or in kana by
level (UserSettings japaneseNotationLevel, 1-5; 3 the default).

The rows of notation-rules.tsv are matched against Mozc's dictionary: two
words of the same reading and part of speech that differ only by a row's
kanji and kana parts. Auxiliary verbs and formal nouns (動詞,非自立 and
名詞,非自立: 下さい, 事) written both ways are found by their part of speech
and take level 3, as the 2010 directive writes them in kana.

Writes <out>/notation.tsv: part of speech (left id) TAB word TAB level TAB
K (the kanji form) or k (the kana form).

  python build-japanese-notation.py --mozc <mozc dir> --out data/ja/japanese-notation
"""
from __future__ import annotations

import argparse
import hashlib
import json
from collections import defaultdict
from pathlib import Path

RULES = Path(__file__).with_name("notation-rules.tsv")
# The auxiliary verbs and formal nouns the directive writes in kana, by the
# base form of their kanji part of speech (not 出す, 切る, 過ぎる: 読み始める).
AUXILIARIES = {"行く", "来る", "置く", "見る", "仕舞う", "上げる", "頂く", "下さる", "参る", "致す",
               "事", "物", "時", "所", "訳", "筈", "程", "為", "様", "通り", "外", "他", "故", "共"}
# Written in kana by about half of web text (とき 48%): a level higher.
AUXILIARY_LEVEL = {"時": 4}


def kanji(text: str) -> bool:
    return any("一" <= c <= "鿿" or c == "々" for c in text)


def kana(text: str) -> bool:
    return all("ぁ" <= c <= "ゟ" or c == "ー" for c in text)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mozc", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--show", action="store_true", help="print the words found by part of speech")
    args = parser.parse_args()
    oss = args.mozc / "dictionary_oss"
    names = [line.split(" ", 1)[1] for line in (oss / "id.def").read_text(encoding="utf-8").splitlines()]
    groups: dict[tuple[str, int], set[str]] = defaultdict(set)
    for path in sorted(oss.glob("dictionary*.txt")):
        for line in path.read_text(encoding="utf-8").splitlines():
            f = line.split("\t")
            if len(f) == 5:
                groups[(f[0], int(f[1]))].add(f[4])
    rules = []
    for line in RULES.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            k, kn, level = line.split("\t")
            rules.append((k, kn, int(level)))
    found: dict[tuple[int, str], tuple[int, str]] = {}
    shown = []
    for (reading, left), surfaces in groups.items():
        for k, kn, level in rules:
            for s in surfaces:
                if k in s:
                    other = s.replace(k, kn, 1)
                    if other in surfaces:
                        found.setdefault((left, s), (level, "K"))
                        found.setdefault((left, other), (level, "k"))
    # Auxiliaries: the kanji and kana forms have parts of speech of their own
    # that differ only by the base form (下さる, くださる).
    by_pos: dict[tuple[str, str], list[tuple[int, str]]] = defaultdict(list)
    for (reading, left), surfaces in groups.items():
        pos = names[left]
        if pos.startswith(("名詞,非自立", "動詞,非自立")):
            for s in surfaces:
                f = pos.split(",")
                by_pos[(reading, f[0], f[1], f[2], f[5])].append((left, s))
    for (reading, *_), words in by_pos.items():
        kana_forms = [(left, s) for left, s in words if s == reading]
        if not kana_forms or not any(names[left].rsplit(",", 1)[1] in AUXILIARIES for left, _ in words):
            continue
        for left, s in words:
            if kanji(s) and names[left].rsplit(",", 1)[1] in AUXILIARIES:
                if (left, s) not in found:
                    shown.append(f"{s}\t{reading}\t{pos}")
                found.setdefault((left, s), (AUXILIARY_LEVEL.get(names[left].rsplit(",", 1)[1], 3), "K"))
        level = min((AUXILIARY_LEVEL.get(names[left].rsplit(",", 1)[1], 3) for left, s in words if kanji(s)), default=3)
        for left, s in kana_forms:
            found.setdefault((left, s), (level, "k"))
    if args.show:
        print("\n".join(sorted(shown)))
    args.out.mkdir(parents=True, exist_ok=True)
    out = args.out / "notation.tsv"
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        for (left, word), (level, side) in sorted(found.items()):
            f.write(f"{left}\t{word}\t{level}\t{side}\n")
    (args.out / "NOTICE").write_text(
        "TEKITO Japanese Notation\n\n"
        "Which words are written in kanji or in kana at each level. The rows were\n"
        "written for TEKITO after 公用文における漢字使用等について (平成22年内閣訓令第1号)\n"
        "and 公用文作成の考え方 (令和4年), which are not subject to copyright\n"
        "(Copyright Act of Japan, Article 13). The parts of speech and words they\n"
        "are matched against come from Mozc (BSD-3-Clause, Copyright 2010-2018\n"
        "Google Inc.).\n", encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": "japanese-notation", "display_name": "Japanese Notation", "schema_version": 1,
        "version": "2026.11", "language": "ja-JP", "type": "japanese-notation", "format": "tsv",
        "file": "notation.tsv", "entry_count": len(found), "sha256": {"file": sha256(out)},
        "license": "TEKITO-OWNED + BSD-3-Clause (Mozc)", "ai_training": True,
        "source": "scripts/ja/build-japanese-notation.py, scripts/ja/notation-rules.tsv; parts of speech and words from https://github.com/google/mozc",
        "notice_file": "NOTICE",
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"japanese-notation: {len(found)} words")


if __name__ == "__main__":
    main()
