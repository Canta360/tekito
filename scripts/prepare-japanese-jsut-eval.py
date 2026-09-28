#!/usr/bin/env python3
"""Builds a larger Japanese conversion eval set from the JSUT corpus labels.

jsut-label (sarulab-speech) gives 5,000 sentences (basic5000) with their
pronunciation in hiragana, written as spoken: は as わ, よう as よー. A
conversion reading is what is typed, so each sentence is split into Mozc
dictionary words whose readings, spoken the same way, give that
pronunciation; the words' own readings then make the typed reading. Sentences
that cannot be split so are left out.

Writes eval/generated/japanese_eval_jsut.tsv, in the layout of
japanese_eval.tsv (source, id, context, reading, expected), and
japanese_eval_jsut_keys.tsv with the romaji keys (see prepare-japanese-eval.py).
The labels are downloaded at a pinned commit into .cache/tekito-data; the
text is CC BY-SA / CC BY (see jsut-label's LICENCE.txt) and only used here.
"""

from __future__ import annotations

import importlib.util
import re
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache" / "tekito-data"
JSUT_COMMIT = "1978271ca6212e1ea742da8f149160f5679e8971"
JSUT = CACHE / "jsut-label" / "basic5000.yaml"
JSUT_URL = f"https://raw.githubusercontent.com/sarulab-speech/jsut-label/{JSUT_COMMIT}/text_kana/basic5000.yaml"
MOZC_COMMIT = "b9c3fcbd6d76b19649ef572324fa9da2559bc18e"
MOZC = CACHE / f"mozc-{MOZC_COMMIT}" / "dictionary_oss"
OUTPUT = ROOT / "eval" / "generated" / "japanese_eval_jsut.tsv"
KEYS_OUTPUT = ROOT / "eval" / "generated" / "japanese_eval_jsut_keys.tsv"
LONGEST_WORD = 12
PUNCTUATION = "、。，．！？「」『』（）・…―－"

VOWEL_ROWS = {
    "あ": "あかさたなはまやらわがざだばぱぁゃゎ",
    "い": "いきしちにひみりぎじぢびぴぃ",
    "う": "うくすつぬふむゆるぐずづぶぷぅゅゔ",
    "え": "えけせてねへめれげぜでべぺぇ",
    "お": "おこそとのほもよろをごぞどぼぽぉょ",
}


def vowel_of(kana: str) -> str:
    return next((v for v, row in VOWEL_ROWS.items() if kana in row), "")


def hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def spoken(reading: str) -> str:
    """How a reading sounds, loosely, so typed and spoken readings meet:
    は/へ/を as わ/え/お, いう as ゆう, long vowels written out, ou and ei as
    oo and ee."""
    text = hiragana(reading).replace("いう", "ゆう").translate(str.maketrans("はへをぢづ", "わえおじず"))
    out = ""
    for c in text:
        previous = vowel_of(out[-1]) if out else ""
        if c == "ー":
            out += previous
        elif c == "う" and previous == "お":
            out += "お"
        elif c == "い" and previous == "え":
            out += "え"
        else:
            out += c
    return out


def fetch() -> Path:
    if not JSUT.exists():
        JSUT.parent.mkdir(parents=True, exist_ok=True)
        print(f"downloading {JSUT_URL}")
        with urllib.request.urlopen(JSUT_URL) as response:
            JSUT.write_bytes(response.read())
    return JSUT


def sentences():
    """(id, text, spoken kana) from basic5000.yaml, level 0."""
    current: dict[str, str] = {}
    name = None
    for line in fetch().read_text(encoding="utf-8").splitlines():
        if not line.startswith(" ") and line.endswith(":"):
            if name and "text_level0" in current and "kana_level0" in current:
                yield name, current["text_level0"], current["kana_level0"]
            name, current = line[:-1], {}
            continue
        match = re.match(r"\s+(\w+):\s*(.*)$", line)
        if match:
            current[match.group(1)] = match.group(2)
    if name and "text_level0" in current and "kana_level0" in current:
        yield name, current["text_level0"], current["kana_level0"]


def load_words() -> dict[str, dict[str, int]]:
    """Surface -> reading -> the cheapest cost of the Mozc words."""
    words: dict[str, dict[str, int]] = {}
    for path in sorted(MOZC.glob("dictionary0*.txt")):
        for line in path.read_text(encoding="utf-8").splitlines():
            fields = line.split("\t")
            if len(fields) < 5:
                continue
            reading, cost, surface = fields[0], int(fields[3]), fields[4]
            readings = words.setdefault(surface, {})
            readings[reading] = min(cost, readings.get(reading, cost))
    return words


def split(text: str, kana: str, words: dict[str, dict[str, int]]) -> str | None:
    """The typed reading of `text` whose spoken form is `kana`, or None."""
    target = spoken(kana.replace("、", "").replace("，", ""))
    n, m = len(text), len(target)
    # best[i][j]: cheapest (cost, reading) covering text[:i] and target[:j].
    best: list[dict[int, tuple[int, str]]] = [dict() for _ in range(n + 1)]
    best[0][0] = (0, "")
    for i in range(n):
        for j, (cost, reading) in list(best[i].items()):
            c = text[i]
            if c in PUNCTUATION:
                # Commas are typed; other marks are left out of the reading.
                typed = "、" if c in "、，" else ""
                candidate = (cost, reading + typed)
                if j not in best[i + 1] or candidate[0] < best[i + 1][j][0]:
                    best[i + 1][j] = candidate
                continue
            options = []
            for length in range(1, min(LONGEST_WORD, n - i) + 1):
                surface = text[i:i + length]
                # Kana is typed as written (いう, not ゆう).
                if re.fullmatch(r"[ぁ-ゖー]+", surface):
                    options.append((length, surface, words.get(surface, {}).get(surface, 9000)))
                    continue
                if re.fullmatch(r"[ァ-ヶー]+", surface):
                    options.append((length, hiragana(surface), words.get(surface, {}).get(hiragana(surface), 8000)))
                    continue
                for word_reading, word_cost in words.get(surface, {}).items():
                    options.append((length, word_reading, word_cost))
            for length, word_reading, word_cost in options:
                sound = spoken(word_reading)
                if target.startswith(sound, j):
                    key = j + len(sound)
                    candidate = (cost + word_cost, reading + word_reading)
                    if key not in best[i + length] or candidate[0] < best[i + length][key][0]:
                        best[i + length][key] = candidate
    end = best[n].get(m)
    return end[1] if end else None


def main() -> int:
    spec = importlib.util.spec_from_file_location("prepare_eval", Path(__file__).with_name("prepare-japanese-eval.py"))
    prepare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prepare)
    keys_for = prepare.romaji_table()

    words = load_words()
    kept = keyed = total = 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with OUTPUT.open("w", encoding="utf-8", newline="\n") as out, \
            KEYS_OUTPUT.open("w", encoding="utf-8", newline="\n") as keys_out:
        for name, text, kana in sentences():
            total += 1
            expected = text.rstrip("。．")
            reading = split(expected, kana, words)
            if not reading:
                continue
            out.write(f"jsut\t{name}\t\t{reading}\t{expected}\n")
            kept += 1
            keys = prepare.romanize(reading, keys_for)
            if keys:
                keys_out.write(f"jsut\t{name}\t{keys}\t{expected}\n")
                keyed += 1
    print(f"wrote {kept} of {total} sentences to {OUTPUT} ({keyed} with keys)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
