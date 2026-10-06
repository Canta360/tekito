"""Turns sentences written as kana chunks into a keys file for
`tekito_ja_eval --chunks`, typing the kana as romaji itself, so the keys
are right however the sentences were written.

Input: JSON lines, one sentence each:

  {"text": "今日はGitHubに…", "chunks": ["きょうは", "@github", "に", …]}

A chunk starting with @ is an English word, typed as it is. Each Japanese
chunk's keys are checked to read back as its kana with the japanese-romaji
table; a sentence with one that does not is left out.

  python eval/ja/kana-to-keys.py sentences.jsonl out_keys.tsv [category]
"""

from __future__ import annotations

import importlib.util
import json
import re
import sys
from pathlib import Path

ROMAJI = {
    "あ": "a", "い": "i", "う": "u", "え": "e", "お": "o",
    "か": "ka", "き": "ki", "く": "ku", "け": "ke", "こ": "ko",
    "さ": "sa", "し": "shi", "す": "su", "せ": "se", "そ": "so",
    "た": "ta", "ち": "chi", "つ": "tsu", "て": "te", "と": "to",
    "な": "na", "に": "ni", "ぬ": "nu", "ね": "ne", "の": "no",
    "は": "ha", "ひ": "hi", "ふ": "fu", "へ": "he", "ほ": "ho",
    "ま": "ma", "み": "mi", "む": "mu", "め": "me", "も": "mo",
    "や": "ya", "ゆ": "yu", "よ": "yo",
    "ら": "ra", "り": "ri", "る": "ru", "れ": "re", "ろ": "ro",
    "わ": "wa", "を": "wo", "ん": "nn",
    "が": "ga", "ぎ": "gi", "ぐ": "gu", "げ": "ge", "ご": "go",
    "ざ": "za", "じ": "ji", "ず": "zu", "ぜ": "ze", "ぞ": "zo",
    "だ": "da", "ぢ": "di", "づ": "du", "で": "de", "ど": "do",
    "ば": "ba", "び": "bi", "ぶ": "bu", "べ": "be", "ぼ": "bo",
    "ぱ": "pa", "ぴ": "pi", "ぷ": "pu", "ぺ": "pe", "ぽ": "po",
    "ゔ": "vu",
    "ぁ": "xa", "ぃ": "xi", "ぅ": "xu", "ぇ": "xe", "ぉ": "xo", "ゃ": "xya", "ゅ": "xyu", "ょ": "xyo",
    "ー": "-", "。": ".", "、": ",", "？": "?", "！": "!",
}
# Two kana typed as one (きょ: kyo).
DIGRAPHS = {
    "しゃ": "sha", "しゅ": "shu", "しょ": "sho", "しぇ": "she", "ちゃ": "cha", "ちゅ": "chu", "ちょ": "cho",
    "ちぇ": "che", "じゃ": "ja", "じゅ": "ju", "じょ": "jo", "じぇ": "je", "てぃ": "thi", "でぃ": "dhi",
    "でゅ": "dhu", "ふぁ": "fa", "ふぃ": "fi", "ふぇ": "fe", "ふぉ": "fo", "うぃ": "wi", "うぇ": "we",
    "ゔぁ": "va", "ゔぃ": "vi", "ゔぇ": "ve", "ゔぉ": "vo",
}
for row, consonant in (("きぎにひびぴみり", "k g n h b p m r"),):
    for kana, c in zip(row, consonant.split()):
        for small, vowel in (("ゃ", "a"), ("ゅ", "u"), ("ょ", "o")):
            DIGRAPHS[kana + small] = f"{c}y{vowel}"


def hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def to_keys(kana: str) -> str | None:
    kana = hiragana(kana)
    out = []
    i = 0
    double = False
    while i < len(kana):
        if kana[i] == "っ":
            double = True
            i += 1
            continue
        piece = DIGRAPHS.get(kana[i:i + 2])
        step = 2
        if piece is None:
            piece, step = ROMAJI.get(kana[i]), 1
        if piece is None:
            return None
        if piece == "nn":
            # ん before a consonant other than n, y: one n is enough.
            nxt = kana[i + 1:i + 2]
            following = DIGRAPHS.get(kana[i + 1:i + 3]) or ROMAJI.get(nxt, "")
            if following and following[0] not in "aiueony.,?!-" and nxt != "っ":
                piece = "n"
        if double:
            if not piece[0].isalpha() or piece[0] in "aiueon":
                out.append("xtu")
            else:
                piece = piece[0] + piece
            double = False
        out.append(piece)
        i += step
    if double:
        out.append("xtu")
    return "".join(out)


def load_romaji():
    path = Path(__file__).resolve().parents[2] / "scripts" / "en" / "build-expression-packs.py"
    spec = importlib.util.spec_from_file_location("expression_packs", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.Romaji()


def main() -> int:
    source, out = Path(sys.argv[1]), Path(sys.argv[2])
    category = sys.argv[3] if len(sys.argv) > 3 else "ja"
    romaji = load_romaji()
    marks = {".": "。", ",": "、", "!": "！", "?": "？", "-": "ー"}
    kept, dropped = [], 0
    for number, line in enumerate(source.read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
            keys = []
            # English written inside a kana chunk (このcluster) is a chunk of its own.
            chunks = []
            for chunk in row["chunks"]:
                chunk = chunk.strip()
                if chunk.startswith("@"):
                    chunks.append(chunk)
                    continue
                parts = [part for part in re.split(r"([a-z]+)", chunk) if part]
                chunks += ["@" + part if re.fullmatch(r"[a-z]+", part) else part for part in parts]
            for chunk in chunks:
                chunk = chunk.strip()
                if chunk.startswith("@"):
                    if not re.fullmatch(r"[a-z]+", chunk[1:]):
                        raise ValueError(f"english {chunk!r}")
                    keys.append(chunk[1:] + "*")
                    continue
                typed = to_keys(chunk)
                if typed is None:
                    raise ValueError(f"cannot type {chunk!r}")
                letters = re.sub(r"[.,!?]+$", "", typed)
                tail = "".join(marks[c] for c in typed[len(letters):])
                read = romaji.kana(letters) if letters else ""
                if read is None or read + tail != hiragana(chunk):
                    raise ValueError(f"{typed!r} reads {read!r}, not {chunk!r}")
                keys.append(typed)
            kept.append("\t".join([f"fresh-{category}", str(number), " ".join(keys), row["text"].strip()]))
        except (KeyError, ValueError, TypeError, json.JSONDecodeError) as error:
            dropped += 1
            print(f"  left out {number}: {error}", file=sys.stderr)
    out.write_text("\n".join(kept) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(kept)} sentences kept, {dropped} left out -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
