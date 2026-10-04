#!/usr/bin/env python3
"""Builds the Data Packs that come from TEKITO's own work rather than from a
downloaded dataset:

  slang                chat abbreviations and informal spellings TEKITO
                       keeps as typed, from slang-seeds.tsv
  social-expression    emoji, emoticons, kaomoji, symbols and short forms for
                       chat words, from social-expression-seeds.tsv
  japanese-phonetic    English words typed the way they sound in Japanese
                       ("konpyuutaa" -> computer), generated from the CMU
                       Pronouncing Dictionary in the pronunciation pack
  qwerty-typo-catalog  synthetic typos of common words, for evaluation only
  japanese-loanwords   English words by how they sound in katakana
                       (ミーティング -> meeting), from japanese-phonetic

The pronunciation, frequency and standard-english packs must already be in
data/. The output is deterministic: running it twice gives identical packs.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import itertools
import json
import re
import shutil
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from datapacks import DATA, manifests, pack_dir  # noqa: E402
SEEDS = Path(__file__).with_name("social-expression-seeds.tsv")
SLANG_SEEDS = Path(__file__).with_name("slang-seeds.tsv")
VERSION = "2026.09"


# ---------------------------------------------------------------------------
# Pack files


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def write_index(data_path: Path, index_path: Path, stride: int = 1024) -> int:
    """Byte offsets of every stride-th row, keyed by its first field."""
    count = 0
    previous = b""
    with data_path.open("rb") as source, index_path.open("wb") as output:
        output.write(b"TEKITO_SLANG_INDEX_V1\n")
        while True:
            offset = source.tell()
            line = source.readline()
            if not line:
                break
            key = line.rstrip(b"\r\n").split(b"\t", 1)[0]
            if key < previous:
                raise ValueError(f"rows are not sorted: {data_path}")
            previous = key
            if count % stride == 0:
                output.write(str(offset).encode("ascii") + b"\t" + key + b"\n")
            count += 1
    return count


def new_pack(pack_id: str) -> Path:
    pack = pack_dir(pack_id)
    if pack.exists():
        shutil.rmtree(pack)
    pack.mkdir(parents=True)
    return pack


def finish(pack: Path, display_name: str, pack_type: str, data_file: str, license_id: str,
           source: str, notice: str, count: int, indexed: bool = True, **extra) -> dict:
    index_file = f"{data_file}.idx"
    if indexed:
        write_index(pack / data_file, pack / index_file)
    else:
        (pack / index_file).write_text("TEKITO_INPUT_CATALOG_INDEX_V1\n0\tcatalog\n",
                                       encoding="ascii", newline="\n")
    (pack / "NOTICE").write_text(notice.strip() + "\n", encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": pack.name,
        "display_name": display_name,
        "schema_version": 1,
        "version": VERSION,
        "language": "en-US",
        "type": pack_type,
        "format": "indexed-tsv-v1" if indexed else "catalog-jsonl-v1",
        "file": data_file,
        "index_file": index_file,
        "entry_count": count,
        "sha256": {"file": sha256(pack / data_file), "index": sha256(pack / index_file)},
        "license": license_id,
        "source": source,
        "notice_file": "NOTICE",
        **extra,
    }
    (pack / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                        encoding="utf-8", newline="\n")
    return manifest


def read_first_fields(path: Path) -> dict[str, str]:
    rows = {}
    with path.open(encoding="utf-8") as source:
        for line in source:
            if line.startswith("#") or not line.strip():
                continue
            fields = line.rstrip("\n").split("\t")
            rows.setdefault(fields[0], fields[1] if len(fields) > 1 else "")
    return rows


def common_words(limit: int) -> list[str]:
    """The most frequent plain lowercase words of the standard lexicon."""
    lexicon = set(read_first_fields(pack_dir("standard-english") / "lexicon.txt"))
    scores = read_first_fields(pack_dir("frequency") / "word-scores.tsv")
    words = [word for word in lexicon
             if re.fullmatch(r"[a-z]{2,}", word) and word in scores]
    words.sort(key=lambda word: (-float(scores[word]), word))
    return words[:limit]


# ---------------------------------------------------------------------------
# slang


def build_slang() -> dict:
    rows = set()
    for number, line in enumerate(SLANG_SEEDS.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3 or fields[2] not in ("abbreviation", "colloquial", "slang"):
            raise ValueError(f"{SLANG_SEEDS.name}:{number}: expected spelling, meaning, label")
        rows.add((fields[0].lower(), fields[1], fields[2]))
    pack = new_pack("slang")
    with (pack / "entries.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for spelling, meaning, label in sorted(rows):
            output.write(f"{spelling}\t{meaning}\t{label}\ten-US\tTEKITO-owned\n")
    return finish(
        pack, "TEKITO Slang", "slang-abbreviation", "entries.tsv", "TEKITO-OWNED",
        "scripts/en/slang-seeds.tsv",
        """
Chat abbreviations and informal spellings that people type on purpose, with
what they mean.

Copyright 2026 Capitata. Written for TEKITO and covered by the TEKITO
source license (LICENSE.md in the TEKITO repository). No third-party corpus
is included in this pack.

TEKITO never corrects a spelling listed here; the meaning is offered as a
suggestion only.
""", len(rows))


# ---------------------------------------------------------------------------
# social-expression

RANGE_BY_KIND = {
    "text_expansion": "common",
    "emoticon": "familiar",
    "kaomoji": "familiar",
    "symbol": "broad",
    "emoji": "broad",
}


def build_social() -> dict:
    rows = set()
    for number, line in enumerate(SEEDS.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3 or fields[2] not in RANGE_BY_KIND:
            raise ValueError(f"{SEEDS.name}:{number}: expected trigger, expression, kind")
        trigger, expression, kind = fields
        label = "emoji" if kind == "emoji" else "social"
        rows.add((trigger.lower(), expression, label, RANGE_BY_KIND[kind],
                  f"kind={kind};policy=suggest_only;raw_pinned=true;auto_apply=false"))
    pack = new_pack("social-expression")
    with (pack / "entries.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for trigger, expression, label, tier, notes in sorted(rows):
            output.write(f"{trigger}\t{expression}\t{label}\ten-US\t{tier}\t{notes}\n")
    return finish(
        pack, "Social Expressions", "social-expression", "entries.tsv", "TEKITO-OWNED",
        "scripts/en/social-expression-seeds.tsv",
        """
Emoji, emoticons, kaomoji, symbols and short forms for chat words.

The mapping from words to expressions was written for TEKITO and is covered
by the TEKITO source license (LICENSE.md in the TEKITO repository). The
emoji and symbols themselves are Unicode characters.

Every entry is a suggestion only; TEKITO never replaces a word with one.
""", len(rows))


# ---------------------------------------------------------------------------
# japanese-phonetic
#
# CMU Pronouncing Dictionary phonemes are turned into the romaji a Japanese
# speaker would type for the katakana form of the word: computer
# (K AH0 M P Y UW1 T ER0) -> konpyuutaa. Where Japanese usage varies, every
# common form is produced (ti/chi, ee/ei, with and without long vowels).

VOWELS = {"AA", "AE", "AH", "AO", "AW", "AY", "EH", "ER", "EY", "IH", "IY", "OW", "OY", "UH", "UW"}
CONSONANT_ROMAJI = {
    "B": "b", "CH": "ch", "D": "d", "DH": "z", "F": "f", "G": "g", "HH": "h", "JH": "j",
    "K": "k", "L": "r", "M": "m", "N": "n", "P": "p", "R": "r", "S": "s", "SH": "sh",
    "T": "t", "TH": "s", "V": "b", "Z": "z", "ZH": "j", "NG": "n",
}
# Short vowels that double a following final stop: cat -> kyatto.
SHORT_VOWELS = {"AE", "EH", "IH", "UH", "AH", "AA"}


def spelling_vowels(word: str) -> list[str]:
    groups = re.findall(r"[aeiouy]+", word)
    if len(groups) > 1 and word.endswith("e") and groups[-1] == "e":
        groups.pop()  # silent final e
    return groups


def vowel_options(phoneme: str, stress: str, letter: str) -> list[str]:
    if phoneme == "AH":
        if stress != "0":
            return ["a", "o"] if letter == "o" else ["a"]  # love -> rabu, london -> rondon
        hinted = {"o": "o", "e": "e", "i": "i", "u": "a", "y": "i"}.get(letter, "a")
        return list(dict.fromkeys([hinted, "a"]))
    if phoneme == "AA":
        return ["o"] if letter == "o" else ["aa", "a"]
    return {
        "AE": ["a"], "AO": ["oo", "o"], "AW": ["au"], "AY": ["ai"], "EH": ["e"],
        "ER": ["aa", "a"], "EY": ["ee", "ei"], "IH": ["i"], "IY": ["ii", "i"],
        "OW": ["oo", "ou", "o"], "OY": ["oi"], "UH": ["u"], "UW": ["uu", "u"],
    }[phoneme]


def syllable(consonant: str, vowel: str) -> list[str]:
    """Romaji for one consonant (or "", "y", "w") before a vowel string."""
    first, rest = vowel[0], vowel[1:]
    if consonant == "s" and first == "i":
        return ["shi" + rest]
    if consonant == "z" and first == "i":
        return ["ji" + rest]
    if consonant == "t" and first == "i":
        return ["ti" + rest, "chi" + rest]
    if consonant == "d" and first == "i":
        return ["di" + rest, "ji" + rest]
    if consonant == "t" and first == "u":
        return ["tsu" + rest, "tu" + rest]
    if consonant == "d" and first == "u":
        return ["du" + rest, "zu" + rest]
    if consonant == "h" and first == "u":
        return ["fu" + rest]
    if consonant in ("ch", "sh", "j") and first == "i":
        return [consonant + vowel]
    if consonant == "y":
        return {"i": ["i" + rest], "e": ["ie" + rest, "ye" + rest]}.get(first, ["y" + vowel])
    if consonant == "w":
        return {"i": ["ui" + rest, "wi" + rest], "e": ["ue" + rest, "we" + rest],
                "o": ["wo" + rest, "uo" + rest], "u": ["u" + rest]}.get(first, ["w" + vowel])
    return [consonant + vowel]


def final_consonant(consonant: str, doubled: bool) -> list[str]:
    """Romaji for a consonant with no vowel after it."""
    if consonant in ("m", "n"):
        return ["n"]
    if consonant == "ch":
        return ["tchi" if doubled else "chi"]
    if consonant == "j":
        return ["ji"]
    if consonant == "sh":
        return ["sshu" if doubled else "shu"]
    if consonant in ("t", "d"):
        return [("t" if consonant == "t" else "d") * (2 if doubled else 1) + "o"]
    lead = consonant[0] * 2 if doubled and consonant in ("p", "k", "g") else consonant
    return [lead + "u"] if consonant != "h" else ["fu"]


def romaji_forms(word: str, phonemes: list[str]) -> list[str]:
    # An r-colored vowel before another vowel is a vowel plus r:
    # arrival (ER0 AY1 ...) -> a + rai...
    expanded = []
    for index, phoneme in enumerate(phonemes):
        following = phonemes[index + 1].rstrip("012") if index + 1 < len(phonemes) else None
        if phoneme.startswith("ER") and following in VOWELS:
            expanded += ["AH" + phoneme[2:], "R"]
        else:
            expanded.append(phoneme)
    phonemes = expanded
    # The last letter of a spelling vowel group is the better hint:
    # station -> "io" -> o (suteeshon), famous -> "ou" -> u (feemasu).
    letters = [group[-1] for group in spelling_vowels(word)]
    vowel_count = sum(1 for p in phonemes if p.rstrip("012") in VOWELS)
    use_letters = len(letters) == vowel_count
    parts: list[list[str]] = []
    vowel_index = 0
    i = 0
    while i < len(phonemes):
        base, stress = phonemes[i].rstrip("012"), phonemes[i][len(phonemes[i].rstrip("012")):]
        if base in VOWELS:
            letter = letters[vowel_index][0] if use_letters else ""
            vowel_index += 1
            parts.append(vowel_options(base, stress, letter))
            i += 1
            continue
        nxt = phonemes[i + 1].rstrip("012") if i + 1 < len(phonemes) else None
        after = phonemes[i + 2].rstrip("012") if i + 2 < len(phonemes) else None
        if base == "R" and nxt not in VOWELS:
            i += 1  # "car" -> kaa: a vowel before a silent r is already long
            continue
        if base in ("Y", "W"):
            if nxt in VOWELS:
                letter = letters[vowel_index][0] if use_letters else ""
                vowel_index += 1
                stress_digit = phonemes[i + 1][len(nxt):]
                options = [s for v in vowel_options(nxt, stress_digit, letter)
                           for s in syllable(base.lower(), v)]
                parts.append(list(dict.fromkeys(options)))
                i += 2
            else:
                parts.append(["i" if base == "Y" else "u"])
                i += 1
            continue
        consonant = CONSONANT_ROMAJI[base]
        if base == "NG":
            # singer -> shingaa, king -> kingu, thank -> sanku
            parts.append(["n"])
            if nxt in VOWELS:
                consonant = "g"
            else:
                if nxt is None:
                    parts.append(["gu"])
                i += 1
                continue
        if base == "L" and nxt not in VOWELS:
            parts.append(["ru"])  # ball -> booru, help -> herupu
            i += 1
            continue
        if nxt == "Y" and after in VOWELS:
            # Palatal: p + y + uu -> pyuu.
            letter = letters[vowel_index][0] if use_letters else ""
            vowel_index += 1
            stress_digit = phonemes[i + 2][len(after):]
            parts.append([consonant + "y" + v for v in vowel_options(after, stress_digit, letter)])
            i += 3
            continue
        if nxt == "W" and after in VOWELS and base in ("K", "G"):
            # quick -> kuikku, queen -> kuiin
            letter = letters[vowel_index][0] if use_letters else ""
            vowel_index += 1
            stress_digit = phonemes[i + 2][len(after):]
            parts.append([consonant + "u" + v for v in vowel_options(after, stress_digit, letter)])
            i += 3
            continue
        if nxt in VOWELS:
            letter = letters[vowel_index][0] if use_letters else ""
            vowel_index += 1
            stress_digit = phonemes[i + 1][len(nxt):]
            vowels = vowel_options(nxt, stress_digit, letter)
            if nxt == "AE" and consonant in ("k", "g"):
                vowels = ["ya", "a"]  # cat -> kyatto
            parts.append(list(dict.fromkeys(s for v in vowels for s in syllable(consonant, v))))
            i += 2
            continue
        if base == "M" and nxt is None:
            parts.append(["mu"])  # game -> geemu
            i += 1
            continue
        previous = phonemes[i - 1].rstrip("012") if i > 0 else None
        doubled = nxt is None and previous in SHORT_VOWELS
        parts.append(final_consonant(consonant, doubled))
        i += 1
    forms = []
    for combination in itertools.islice(itertools.product(*parts), 8):
        forms.append("".join(combination))
    # People often skip long vowels when typing: konpyuta.
    short = re.sub(r"([aiueo])\1", r"\1", forms[0])
    forms.append(short.replace("ou", "o").replace("ei", "e"))
    return list(dict.fromkeys(forms))


def build_japanese_phonetic(word_count: int) -> dict:
    lexicon = set(read_first_fields(pack_dir("standard-english") / "lexicon.txt"))
    pronunciations = read_first_fields(pack_dir("pronunciation") / "pronunciations.tsv")
    rows = set()
    for word in common_words(word_count * 2):
        if word not in pronunciations or len(word) < 3:
            continue
        phonemes = pronunciations[word].split(" | ")[0].split("#")[0].split()
        for form in romaji_forms(word, phonemes):
            # Skip forms that are English words themselves; those would
            # compete with ordinary typing.
            if len(form) >= 3 and form != word and form not in lexicon:
                rows.add((form, word))
        if len({word for _, word in rows}) >= word_count:
            break
    pack = new_pack("japanese-phonetic")
    with (pack / "entries.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for form, word in sorted(rows):
            output.write(f"{form}\t{word}\tphonetic\ten-US\tsource=cmudict\n")
    return finish(
        pack, "Japanese Phonetic Suggestions", "japanese-phonetic", "entries.tsv",
        "CMUdict",
        "scripts/en/build-expression-packs.py from the pronunciation and frequency packs",
        """
English words spelled the way they sound in Japanese romaji, such as
"konpyuutaa" for computer. Generated by scripts/en/build-expression-packs.py.

The pronunciations come from the CMU Pronouncing Dictionary:

  Copyright (C) 1993-2015 Carnegie Mellon University. All rights reserved.
  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the conditions in the
  pronunciation pack's NOTICE are met.

Which words are included was decided by word frequency from the Leipzig
Corpora Collection (English news 2025, 1M sentences), licensed CC BY 4.0:
D. Goldhahn, T. Eckart and U. Quasthoff, "Building Large Monolingual
Dictionaries at the Leipzig Corpora Collection", LREC 2012.

Every entry is a suggestion only; TEKITO never replaces a word with one.
""", len(rows))


# ---------------------------------------------------------------------------
# japanese-english-words


# Hiragana by the vowel they end in, for the long-vowel mark (as in
# src/Core/Japanese/Loanwords.cpp).
VOWELS = {
    "あ": "あかさたなはまやらわがざだばぱぁゃゎ",
    "い": "いきしちにひみりぎじぢびぴぃ",
    "う": "うくすつぬふむゆるぐずづぶぷぅゅゔ",
    "え": "えけせてねへめれげぜでべぺぇ",
    "お": "おこそとのほもよろをごぞどぼぽぉょ",
}


def loanword_key(kana: str) -> str:
    """LoanwordKey in src/Core/Japanese/Loanwords.cpp: how katakana sounds,
    loosely, so ミーティング and the romaji "miitingu" meet."""
    text = "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in kana)
    for spelled, sound in (("てぃ", "ち"), ("でぃ", "じ"), ("ぢ", "じ"), ("づ", "ず"), ("ゔ", "ぶ")):
        text = text.replace(spelled, sound)
    key = ""
    for c in text:
        if c == "ー":
            vowel = next((v for v, row in VOWELS.items() if key and key[-1] in row), "")
            key += vowel
            continue
        key += c
    return key.translate(str.maketrans("ぁぃぅぇぉ", "あいうえお"))


class Romaji:
    """The japanese-romaji table, read the way JapaneseComposer reads keys."""

    def __init__(self) -> None:
        self.rules: dict[str, tuple[str, str]] = {}
        for line in (pack_dir("japanese-romaji") / "romaji.tsv").read_text(encoding="utf-8").splitlines():
            keys, output, pending = line.split("\t")
            self.rules[keys] = (output, pending)
        self.longer = {keys[:i] for keys in self.rules for i in range(1, len(keys))}

    def kana(self, keys: str) -> str | None:
        """The kana `keys` type, or None when a key is left unread."""
        out: list[str] = []
        pending = ""

        def feed(key: str) -> bool:
            nonlocal pending
            for _ in range(64):
                typed = pending + key
                if typed in self.longer:
                    pending = typed
                    return True
                if typed in self.rules:
                    output, keep = self.rules[typed]
                    out.append(output)
                    pending = keep if typed.endswith(keep) else ""
                    return True
                if not pending:
                    return False
                if not flush_once():
                    return False
            return False

        def flush_once() -> bool:
            nonlocal pending
            held, pending = pending, ""
            for length in range(len(held), 0, -1):
                rule = self.rules.get(held[:length])
                if rule and not rule[1]:
                    out.append(rule[0])
                    return all(feed(rest) for rest in held[length:])
            return False

        for key in keys:
            if not feed(key):
                return None
        while pending:
            if not flush_once():
                return None
        return "".join(out)


def build_japanese_loanwords() -> dict:
    scores = read_first_fields(pack_dir("frequency") / "word-scores.tsv")
    romaji = Romaji()
    rows: dict[tuple[str, str], float] = {}
    with (pack_dir("japanese-phonetic") / "entries.tsv").open(encoding="utf-8") as source:
        for line in source:
            form, word = line.rstrip("\n").split("\t")[:2]
            kana = romaji.kana(form)
            if kana and len(kana) >= 2:
                rows[(loanword_key(kana), word)] = float(scores.get(word, 0))
    by_key: dict[str, list[tuple[float, str]]] = {}
    for (key, word), score in rows.items():
        by_key.setdefault(key, []).append((-score, word))
    pack = new_pack("japanese-loanwords")
    count = 0
    with (pack / "words.tsv").open("wb") as output:
        for key in sorted(by_key, key=lambda k: k.encode("utf-8")):
            for score, word in sorted(by_key[key])[:3]:
                output.write(f"{key}\t{word}\t{-score:.4f}\n".encode("utf-8"))
                count += 1
    return finish(pack, "Loanwords for Japanese Input", "japanese-loanwords", "words.tsv",
                  "CMUdict + CC-BY-4.0",
                  "scripts/en/build-expression-packs.py from the japanese-phonetic, japanese-romaji and frequency packs",
                  f"""
English words by how they sound in katakana ("みいちんぐ" for meeting), so
Japanese input can offer the English word for a katakana word. Each romaji
spelling of the japanese-phonetic pack is read with the japanese-romaji
table and keyed loosely (long vowels written out, small vowels full size,
ティ as チ); a key's words come most common first.

Pronunciations: the CMU Pronouncing Dictionary (see the japanese-phonetic
pack's NOTICE). Frequencies: Leipzig Corpora Collection (English news 2025,
1M sentences), licensed CC BY 4.0: D. Goldhahn, T. Eckart and U. Quasthoff,
"Building Large Monolingual Dictionaries at the Leipzig Corpora Collection",
LREC 2012.
""", count, language="ja-JP")


# ---------------------------------------------------------------------------
# qwerty-typo-catalog

KEY_ROWS = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
NEIGHBORS: dict[str, str] = {}
for row_index, row in enumerate(KEY_ROWS):
    for column, key in enumerate(row):
        near = [row[c] for c in (column - 1, column + 1) if 0 <= c < len(row)]
        for other_row in (row_index - 1, row_index + 1):
            if 0 <= other_row < len(KEY_ROWS):
                near += [KEY_ROWS[other_row][c] for c in (column - 1, column, column + 1)
                         if 0 <= c < len(KEY_ROWS[other_row])]
        NEIGHBORS[key] = "".join(near)


def stable_choice(text: str, options: int) -> int:
    return int(hashlib.sha1(text.encode("utf-8")).hexdigest()[:8], 16) % options


def typos(word: str) -> list[tuple[str, str]]:
    """A few typical slips for one word, chosen deterministically."""
    results = []
    if len(word) >= 3:
        i = 1 + stable_choice(word + "t", len(word) - 2)
        results.append(("adjacent_transposition", word[:i] + word[i + 1] + word[i] + word[i + 2:]))
    if len(word) >= 4:
        i = 1 + stable_choice(word + "m", len(word) - 1)
        results.append(("missing_character", word[:i] + word[i + 1:]))
    i = stable_choice(word + "r", len(word))
    results.append(("repeated_character", word[:i + 1] + word[i] + word[i + 1:]))
    i = stable_choice(word + "n", len(word))
    near = NEIGHBORS.get(word[i], "")
    if near:
        replacement = near[stable_choice(word + "k", len(near))]
        results.append(("qwerty_neighbor", word[:i] + replacement + word[i + 1:]))
    return results


def build_typo_catalog(word_count: int) -> dict:
    lexicon = set(read_first_fields(pack_dir("standard-english") / "lexicon.txt"))
    records = []
    for word in common_words(word_count):
        split = ("test", "validation", "train", "train", "train")[stable_choice(word, 5)]
        base = {"split": split, "group_key": word, "left_context": "", "right_context": ""}
        records.append({**base, "category": "valid_word", "raw_token": word,
                        "expected_token": word, "expected_action": "keep_raw",
                        "typo_operation": "none"})
        for operation, typo in typos(word):
            if typo == word or typo in lexicon:
                continue  # a slip that makes another real word is not a typo
            records.append({**base, "category": "correction_positive", "raw_token": typo,
                            "expected_token": word, "expected_action": "auto_apply_eligible",
                            "typo_operation": operation})
    for line in SLANG_SEEDS.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        spelling = line.split("\t")[0].lower()
        records.append({"split": "test", "group_key": spelling, "left_context": "",
                        "right_context": "", "category": "chat_abbreviation",
                        "raw_token": spelling, "expected_token": spelling,
                        "expected_action": "keep_raw", "typo_operation": "none"})
    pack = new_pack("qwerty-typo-catalog")
    with gzip.GzipFile(pack / "catalog.jsonl.gz", "wb", mtime=0) as raw_output:
        for number, record in enumerate(records):
            line = json.dumps({"example_id": f"typo-{number:06d}", **record},
                              ensure_ascii=False, separators=(",", ":"))
            raw_output.write((line + "\n").encode("utf-8"))
    return finish(
        pack, "QWERTY Typo Evaluation", "qwerty-typo-evaluation", "catalog.jsonl.gz",
        "TEKITO-OWNED", "scripts/en/build-expression-packs.py",
        """
Synthetic typing slips on a QWERTY keyboard (swapped, missing, doubled and
neighboring keys) for common English words, plus the words typed correctly
and the chat abbreviations from the slang pack, which must stay as typed.
Used only to evaluate TEKITO; it is not read while typing.

Generated by scripts/en/build-expression-packs.py and covered by the TEKITO
source license. Which words are included was decided by word frequency from
the Leipzig Corpora Collection (English news 2025), licensed CC BY 4.0.
""", len(records), indexed=False)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--only", choices=("slang", "social-expression", "japanese-phonetic",
                                           "qwerty-typo-catalog", "japanese-loanwords"))
    parser.add_argument("--phonetic-words", type=int, default=20000)
    parser.add_argument("--typo-words", type=int, default=5000)
    args = parser.parse_args()
    builders = {
        "slang": build_slang,
        "social-expression": build_social,
        "japanese-phonetic": lambda: build_japanese_phonetic(args.phonetic_words),
        "qwerty-typo-catalog": lambda: build_typo_catalog(args.typo_words),
        "japanese-loanwords": build_japanese_loanwords,
    }
    for name, builder in builders.items():
        if args.only in (None, name):
            manifest = builder()
            print(f"{manifest['display_name']}: {manifest['entry_count']:,} entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
