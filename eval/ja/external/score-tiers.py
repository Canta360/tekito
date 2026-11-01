"""Scores conversions against a keys file, allowing more kinds of writing
difference step by step, and finds where the right text is in candidate
sentence lists.

  python eval/ja/external/score-tiers.py tiers KEYS.tsv OUTPUT.tsv [SOURCE]
  python eval/ja/external/score-tiers.py topn  KEYS.tsv SENTENCES.tsv [SOURCE]

KEYS.tsv: source, id, keys, right texts...; OUTPUT.tsv: source, id, text
(tekito_ja_eval with TEKITO_EVAL_OUTPUT=...); SENTENCES.tsv: what
tekito_ja_eval --write-sentences writes (source, id, rank, meant, text; with
TEKITO_EVAL_SENTENCE_ORDER=cost by the converter's costs).

Tiers, each allowing what the ones above it do:
  exact; full/half width (５０, 50); kanji numerals as digits (一日, １日);
  kana or kanji with the same reading, no kanji put for another kanji
  (身体, 体; はじめ, 初め); and, for reference only, any same reading
  (homophones too: 誠意式 for 性意識, which is wrong).

Readings come from SudachiPy (pip install sudachipy sudachidict_core).
"""

from __future__ import annotations

import re
import sys
import unicodedata

from sudachipy import dictionary

TOKENIZER = dictionary.Dictionary().create()
KANJI_DIGIT = dict(zip("〇一二三四五六七八九", range(10)))
UNITS = {"十": 10, "百": 100, "千": 1000}
BIG = {"万": 10**4, "億": 10**8, "兆": 10**12}


def kanji_number(text: str) -> int:
    if all(c in KANJI_DIGIT for c in text):  # 二〇〇六
        return int("".join(str(KANJI_DIGIT[c]) for c in text))
    total = section = digit = 0
    for c in text:
        if c in KANJI_DIGIT:
            digit = KANJI_DIGIT[c]
        elif c in UNITS:
            section += (digit or 1) * UNITS[c]
            digit = 0
        elif c in BIG:
            total += (section + digit) * BIG[c]
            section = digit = 0
    return total + section + digit


def width(text: str) -> str:
    return unicodedata.normalize("NFKC", text)


def numbers(text: str) -> str:
    text = width(text).replace(",", "")
    return re.sub(r"[〇一二三四五六七八九十百千万億兆]+",
                  lambda m: m.group() if m.group() in ("万", "億", "兆") else str(kanji_number(m.group())), text)


def hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


_readings: dict[str, str] = {}


def reading(text: str) -> str:
    if text not in _readings:
        _readings[text] = "".join(hiragana(m.reading_form()) if re.search(r"[぀-鿿]", m.surface()) else m.surface()
                                  for m in TOKENIZER.tokenize(text))
    return _readings[text]


def kanji(text: str) -> str:
    return re.sub(r"[^一-鿿々]", "", text)


def subsequence(a: str, b: str) -> bool:
    it = iter(b)
    return all(c in it for c in a)


def kana_or_kanji(got: str, want: str) -> bool:
    if reading(got) != reading(want):
        return False
    a, b = kanji(got), kanji(want)
    return subsequence(a, b) or subsequence(b, a)


TIERS = [
    ("exact", lambda g, w: g == w),
    ("+ full/half width", lambda g, w: width(g) == width(w)),
    ("+ kanji numerals as digits", lambda g, w: numbers(g) == numbers(w)),
    ("+ kana or kanji, same reading", lambda g, w: kana_or_kanji(numbers(g), numbers(w))),
]
ANY_READING = ("(any same reading, homophones too)", lambda g, w: reading(numbers(g)) == reading(numbers(w)))


def read_keys(path: str, source: str | None) -> dict[tuple[str, str], list[str]]:
    rows = {}
    for line in open(path, encoding="utf-8"):
        p = line.rstrip("\n").split("\t")
        if len(p) >= 4 and (not source or p[0] == source):
            rows[(p[0], p[1])] = p[3:]
    return rows


def tiers(keys: str, outputs: str, source: str | None) -> None:
    want = read_keys(keys, source)
    got = {}
    for line in open(outputs, encoding="utf-8"):
        p = line.rstrip("\n").split("\t")
        if len(p) >= 3:
            got[(p[0], p[1])] = p[2]
    rows = [k for k in want if k in got]
    hits = [0] * len(TIERS)
    homophones = 0
    for k in rows:
        right = False
        for i, (_, test) in enumerate(TIERS):
            right = right or any(test(got[k], w) for w in want[k])
            hits[i] += right
        homophones += any(ANY_READING[1](got[k], w) for w in want[k])
    print(f"rows {len(rows)}")
    for (name, _), hit in zip(TIERS, hits):
        print(f"  {name:36} {100 * hit / len(rows):5.1f}%")
    print(f"  {ANY_READING[0]:36} {100 * homophones / len(rows):5.1f}%")


def topn(keys: str, sentences: str, source: str | None) -> None:
    want = read_keys(keys, source)
    lists: dict[tuple[str, str], list[tuple[int, str]]] = {}
    for line in open(sentences, encoding="utf-8"):
        p = line.rstrip("\n").split("\t")
        if len(p) >= 5 and (p[0], p[1]) in want:
            lists.setdefault((p[0], p[1]), []).append((int(p[2]), p[4]))
    tops = (1, 3, 5, 10, 20, 50)
    print(f"rows {len(want)}: the right text among the first N sentences")
    for name, test in (TIERS[0], TIERS[-1]):
        first = {}
        for k, items in lists.items():
            for rank, text in sorted(items):
                if any(test(text, w) for w in want[k]):
                    first[k] = rank
                    break
        cells = "  ".join(f"top{n}={100 * sum(1 for k in want if first.get(k, 1 << 30) < n) / len(want):5.1f}%"
                          for n in tops)
        print(f"  {name:32} {cells}")


def main() -> int:
    if len(sys.argv) < 4 or sys.argv[1] not in ("tiers", "topn"):
        print(__doc__)
        return 2
    source = sys.argv[4] if len(sys.argv) > 4 else None
    (tiers if sys.argv[1] == "tiers" else topn)(sys.argv[2], sys.argv[3], source)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
