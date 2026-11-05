#!/usr/bin/env python3
"""Fine-tunes a small language model (LiquidAI LFM2.5) to convert kana
readings to Japanese text, for TekitoAI.exe: given what was written before
and the reading, the text. Trained on a GPU (AMD ROCm PyTorch on Windows
works), on the zenz-v2.5 dataset's llm-jp-corpus-v3 part (ODC-BY 1.0, see
fetch-zenz-sample.py).

Each example reads

  U+EE02 left context U+EE00 reading in hiragana U+EE01 text <eos>

with the context left out of some examples (and the U+EE02 with it), so
the model also converts without one. Only the text and <eos> are learned.

  prepare  examples from sample.jsonl, less what any --exclude-dir /
           --exclude-sha1 names (the evaluation sets).
  prepare-keys  the same, but read from the keys typed (romaji as
           kana-to-keys.py types it): some katakana words written in
           English and typed as English letters (Mix), and some keys with
           one slip (a consonant or vowel struck wrong or left out), the
           text staying as meant.
  prepare-english  English sentences (one per line, Leipzig's "id<TAB>text")
           typed with slips: none, one or two a sentence, a letter struck as
           its neighbour, left out, doubled or swapped with the next, or a
           word typed as another real word a slip away; the text as meant.
  prepare-mix  Japanese and English mixed as Mix types it: lines whose
           katakana words have an English form (a loanword, or a name from
           Wikidata's labels, --names) put in English, typed in chunks, with
           a slip in the Japanese, in an English word, both or neither.
  train    fine-tunes --base on them; writes the model to --out/model.
  prepare-rank  TEKITO's conversions of other sentences (tekito_ja_eval
           --write-sentences and TEKITO_EVAL_OUTPUT, in --lists) joined with
           what each meant: the meant text and TEKITO's wrong sentences.
  train-rank  teaches a model from train to choose the meant text among
           them (scored as TekitoAI scores sentences).

  train-japanese-ai-model.py prepare --out DIR --zenz FILE.jsonl [--every N] [--exclude-dir D] [--exclude-sha1 F]
  train-japanese-ai-model.py prepare-keys --out DIR --zenz FILE.jsonl --loanwords words.tsv [--every N] [...]
  train-japanese-ai-model.py prepare-english --out DIR --english sentences.txt --lexicon lexicon.txt [...]
  train-japanese-ai-model.py prepare-mix --out DIR --zenz FILE.jsonl --loanwords words.tsv [--names names.tsv] [...]
  train-japanese-ai-model.py train   --out DIR --base DIR [--examples N] [--lr 5e-5] [--gpu-share 0.8] [--save-every 2000]
  train-japanese-ai-model.py prepare-rank --out DIR --gen rank.gen.tsv --lists DIR [--context rank.context40.tsv]
  train-japanese-ai-model.py train-rank --out DIR --base DIR [--lr 2e-5] [--gpu-share 0.8] [--save-every 2000]
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import os
import random
import time
from pathlib import Path

CONTEXT_TAG, INPUT_TAG, OUTPUT_TAG = "", "", ""
CONTEXT_CHARS = 40
MAX_TEXT_CHARS = 80
NO_CONTEXT_SHARE = 0.3


def hiragana(katakana: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in katakana)


def prompt(context: str, reading: str) -> str:
    start = CONTEXT_TAG + context[-CONTEXT_CHARS:] if context else ""
    return start + INPUT_TAG + reading + OUTPUT_TAG


def excluded_test(args):
    path = Path(__file__).resolve().parent / "train-japanese-wordlm.py"
    spec = importlib.util.spec_from_file_location("wordlm", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.excluded_test(args)


def prepare(args) -> None:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    left_out = excluded_test(args)
    rng = random.Random(0)
    kept = dropped = 0
    with open(out / "examples.jsonl", "w", encoding="utf-8", newline="\n") as f:
        for number, line in enumerate(open(args.zenz, encoding="utf-8")):
            # The file is ordered by where its text came from: every
            # --every'th line samples all of it evenly.
            if number % args.every:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            text, reading = (row.get("output") or "").strip(), (row.get("input") or "").strip()
            if not text or not reading or len(text) > MAX_TEXT_CHARS or "\n" in text:
                continue
            if left_out(text):
                dropped += 1
                continue
            keys = re.sub(r" +", " ", keys)
            f.write(json.dumps({"prompt": prompt(context, hiragana(reading)), "text": text}, ensure_ascii=False) + "\n")
            kept += 1
    print(f"{kept} examples, {dropped} left out as measured")


ENGLISH_SHARE = 0.6   # examples with katakana words put in English
SPACED_SHARE = 0.25   # of those, English typed with a space before and after
SLIP_SHARE = 0.5      # keys examples with one slip in them
READING_SHARE = 0.5   # examples read from the reading instead (as prepare)
CHUNKED_SHARE = 0.25  # keys examples typed in chunks, a space after each (Mix)
PARTICLES = "はがをにでともへのや"
LOANWORD_SCORE = 3.0  # only common English words (words.tsv's score)
VOWEL_SWAP = {"a": "e", "i": "uo", "u": "io", "e": "ai", "o": "iu"}
KEY_ROWS = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]


def load_module(relative: str, name: str):
    path = Path(__file__).resolve().parents[2] / relative
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def neighbours(ch: str) -> list[str]:
    for r, row in enumerate(KEY_ROWS):
        if ch in row:
            i = row.index(ch)
            near = [row[j] for j in (i - 1, i + 1) if 0 <= j < len(row)]
            for other in (r - 1, r + 1):
                if 0 <= other < len(KEY_ROWS):
                    near += [KEY_ROWS[other][j] for j in (i - 1, i, i + 1) if 0 <= j < len(KEY_ROWS[other])]
            return near
    return []


def slip(keys: str, japanese: list[bool], rng: random.Random) -> str | None:
    """One consonant or vowel of the Japanese keys struck as a neighbour
    (a vowel as a likely other vowel) or left out."""
    vowels = [i for i, c in enumerate(keys) if japanese[i] and c in "aiueo"]
    consonants = [i for i, c in enumerate(keys) if japanese[i] and c in "bcdfghjklmpqrstvwxyz"]
    kind = rng.choice(["consonant", "vowel", "consonant-out", "vowel-out"])
    places = consonants if kind.startswith("consonant") else vowels
    if not places:
        return None
    i = rng.choice(places)
    if kind.endswith("-out"):
        return keys[:i] + keys[i + 1:]
    if kind == "vowel":
        return keys[:i] + rng.choice(VOWEL_SWAP[keys[i]]) + keys[i + 1:]
    near = [c for c in neighbours(keys[i]) if c in "bcdfghjklmpqrstvwxyz" and c != keys[i]]
    return keys[:i] + rng.choice(near) + keys[i + 1:] if near else None


# Marks typed as they are in Japanese mode (the romaji table turns them
# into these).
TYPED_MARKS = {"・": "/", "「": "[", "」": "]", "（": "(", "）": ")", "〜": "~", "～": "~", "：": ":",
               "；": ";", "　": " ", "…": "...", "『": "[", "』": "]", "＃": "#", "％": "%", "＆": "&"}


def type_reading(kana_module, reading: str) -> str | None:
    """The keys that type `reading`: kana as romaji; letters, digits and
    marks as the keys that write them; nothing for what cannot be typed."""
    import re
    keys = []
    for run in re.findall(r"[ぁ-ゖー。、？！]+|[^ぁ-ゖー。、？！]+", reading):
        if "ぁ" <= run[0] <= "ゖ" or run[0] in "ー。、？！":
            typed = kana_module.to_keys(run)
            if typed is None:
                return None
            keys.append(typed)
            continue
        for ch in run:
            if "！" <= ch <= "～":
                ch = chr(ord(ch) - 0xFEE0)
            if ch in TYPED_MARKS:
                keys.append(TYPED_MARKS[ch])
            elif " " <= ch <= "~":
                keys.append(ch.lower() if ch.isalpha() else ch)
            else:
                return None
    return "".join(keys)


def prepare_keys(args) -> None:
    import re
    kana = load_module("eval/ja/kana-to-keys.py", "kana_to_keys")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    left_out = excluded_test(args)
    english = {}
    for line in open(args.loanwords, encoding="utf-8"):
        reading, word, score = (line.rstrip("\n").split("\t") + ["", "", "0"])[:3]
        if re.fullmatch(r"[a-z]{2,12}", word) and float(score) >= LOANWORD_SCORE:
            if reading not in english or english[reading][1] < float(score):
                english[reading] = (word, float(score))
    rng = random.Random(0)
    counts = {"kept": 0, "dropped": 0, "untypable": 0, "reading": 0, "english": 0, "chunked": 0, "slips": 0}
    with open(out / "examples.jsonl", "w", encoding="utf-8", newline="\n") as f:
        for number, line in enumerate(open(args.zenz, encoding="utf-8")):
            if number % args.every:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            text, reading = (row.get("output") or "").strip(), hiragana((row.get("input") or "").strip())
            if not text or not reading or len(text) > MAX_TEXT_CHARS or "\n" in text:
                continue
            if left_out(text):
                counts["dropped"] += 1
                continue
            # Addresses and code: their readings spell the letters out.
            if any(mark in text for mark in ("http", "www", "://", "@", ".com", ".jp")) or \
                    sum(c.isascii() and c.isalnum() for c in text) > len(text) / 2:
                counts["untypable"] += 1
                continue
            context = (row.get("left_context") or "").replace("\n", "")
            if rng.random() < NO_CONTEXT_SHARE:
                context = ""
            # Half the examples from the reading, so one model takes either.
            if rng.random() < READING_SHARE:
                f.write(json.dumps({"prompt": prompt(context, reading), "text": text}, ensure_ascii=False) + "\n")
                counts["kept"] += 1
                counts["reading"] += 1
                continue
            # The reading in pieces: (kana or English, is English).
            pieces = [(reading, False)]
            if rng.random() < ENGLISH_SHARE:
                for run in re.findall(r"[ァ-ヴー]{2,}", text):
                    found = english.get(hiragana(run))
                    if not found or rng.random() < 0.3:
                        continue
                    target = hiragana(run)
                    for k, (piece, is_english) in enumerate(pieces):
                        at = piece.find(target) if not is_english else -1
                        if at >= 0:
                            pieces[k:k + 1] = [(piece[:at], False), (found[0], True), (piece[at + len(target):], False)]
                            text = text.replace(run, found[0], 1)
                            counts["english"] += 1
                            break
            chunked = rng.random() < CHUNKED_SHARE
            spaced = chunked or rng.random() < SPACED_SHARE
            if chunked:
                # A space where a chunk would end: after most particles.
                chunks = []
                for piece, is_english in pieces:
                    if not is_english:
                        piece = "".join(c + (" " if c in PARTICLES and i + 1 < len(piece) and rng.random() < 0.6 else "")
                                        for i, c in enumerate(piece))
                    chunks.append((piece, is_english))
                pieces = chunks
                counts["chunked"] += 1
            keys, japanese = "", []
            for piece, is_english in pieces:
                if not piece:
                    continue
                typed = piece if is_english else type_reading(kana, piece)
                if typed is None:
                    keys = None
                    break
                if is_english and spaced:
                    typed = (" " if keys else "") + typed + " "
                keys += typed
                japanese += [not is_english] * len(typed)
            if keys is None or not keys.strip():
                counts["untypable"] += 1
                continue
            keys = keys.strip()
            japanese = japanese[:len(keys)]
            if rng.random() < SLIP_SHARE:
                slipped = slip(keys, japanese, rng)
                if slipped:
                    keys = slipped
                    counts["slips"] += 1
            context = (row.get("left_context") or "").replace("\n", "")
            if rng.random() < NO_CONTEXT_SHARE:
                context = ""
            f.write(json.dumps({"prompt": prompt(context, keys), "text": text}, ensure_ascii=False) + "\n")
            counts["kept"] += 1
    print(counts)


ENGLISH_MAX_CHARS = 160
# How a Mix line is typed: a space after each chunk (particles, English);
# English run into the Japanese around it (meetingde); or only the English
# spaced off.
MIX_TYPING = [("chunked", 0.5), ("joined", 0.3), ("spaced", 0.2)]
MIX_REPLACE_SHARE = 0.8   # each katakana word with an English form put in English
MIX_SLIPS = [("none", 0.35), ("japanese", 0.25), ("english", 0.3), ("both", 0.1)]
MIX_TYPED_CAPITALS = 0.2  # names typed with their capitals (else lower case, as most type them)
NAME_EDITIONS = 30        # Wikidata names in at least this many language editions
MIX_JAPANESE_ONLY = 0.25  # lines with no English kept, as Japanese typed in Mix
VOWEL_KANA = {"a": "あ", "i": "い", "u": "う", "e": "え", "o": "お"}


def long_vowels(kana_module, reading: str) -> str:
    """ー as the vowel it draws out (せんたー: せんたあ), as the loanwords
    pack writes its readings."""
    out: list[str] = []
    for ch in reading:
        if ch == "ー" and out:
            keys = kana_module.to_keys(out[-1]) or ""
            ch = VOWEL_KANA.get(keys[-1:], ch)
        out.append(ch)
    return "".join(out)


def prepare_mix(args) -> None:
    """Japanese and English mixed, as typed in Mix: lines that have English
    in them already (GitHubにpush), lines whose katakana words have an
    English form (a loanword: meeting; a name from Wikidata's labels:
    Boston) put in English, and some lines with no English at all, so the
    model does not mix everything; those always when a chunk spells an
    English word too (same ga oyoida: 鮫). Typed in chunks, run together or
    with only the English spaced off; a slip in the Japanese, in an English
    word, both or neither."""
    import re
    kana = load_module("eval/ja/kana-to-keys.py", "kana_to_keys")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    left_out = excluded_test(args)
    english: dict[str, tuple[str, float]] = {}
    words: set[str] = set()
    loanword_readings: set[str] = set()
    for line in open(args.loanwords, encoding="utf-8"):
        reading, word, score = (line.rstrip("\n").split("\t") + ["", "", "0"])[:3]
        loanword_readings.add(reading)
        if re.fullmatch(r"[a-z]{2,12}", word):
            words.add(word)
            if float(score) >= LOANWORD_SCORE and (reading not in english or english[reading][1] < float(score)):
                english[reading] = (word, float(score))
    # --names: katakana TAB English TAB language editions (Wikidata labels).
    for path in args.names or []:
        for line in open(path, encoding="utf-8"):
            fields = [f.strip('"') for f in line.rstrip("\n").split("\t")]
            if len(fields) < 3 or not fields[2].isdigit():
                continue
            reading = long_vowels(kana, hiragana(fields[0].replace("・", "").replace("＝", "")))
            # Well-known names only, each word capitalized (not Rangifer
            # tarandus), four kana or more (コレ is no Kore), and not a
            # loanword's reading nor a common English word (ページ, Page);
            # of two names, the one with more editions.
            name, links = fields[1], 100.0 + int(fields[2])
            if (int(fields[2]) < NAME_EDITIONS or len(reading) < 4 or reading in loanword_readings
                    or name.lower() in words or not all(w[:1].isupper() for w in name.split(" "))):
                continue
            if reading not in english or english[reading][1] < links:
                english[reading] = (name, links)
    rng = random.Random(0)

    def pick(table: list[tuple[str, float]]) -> str:
        r = rng.random()
        for name, share in table:
            if r < share:
                return name
            r -= share
        return table[-1][0]

    counts = {"kept": 0, "dropped": 0, "untypable": 0, "had-english": 0, "put-in-english": 0,
              "japanese-only": 0, "contrast": 0, "japanese-slips": 0, "english-slips": 0}
    with open(out / "examples.jsonl", "w", encoding="utf-8", newline="\n") as f:
        for number, line in enumerate(open(args.zenz, encoding="utf-8")):
            if number % args.every:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            text, reading = (row.get("output") or "").strip(), hiragana((row.get("input") or "").strip())
            if not text or not reading or len(text) > MAX_TEXT_CHARS or "\n" in text:
                continue
            if any(mark in text for mark in ("http", "www", "://", "@", ".com", ".jp")) or \
                    sum(c.isascii() and c.isalnum() for c in text) > len(text) / 2:
                counts["untypable"] += 1
                continue
            # English already there: a word in letters in both the reading
            # and the text (iPhone, BSL).
            pieces: list[tuple[str, bool]] = []
            start = 0
            for found in re.finditer(r"[A-Za-z][A-Za-z'-]*[A-Za-z]", reading):
                if found.group() in text:
                    pieces += [(reading[start:found.start()], False), (found.group(), True)]
                    start = found.end()
            pieces.append((reading[start:], False))
            had = len(pieces) > 1
            for run in re.findall(r"[ァ-ヴー・＝]{2,}", text):
                found = english.get(long_vowels(kana, hiragana(run.replace("・", "").replace("＝", ""))))
                if not found or rng.random() >= MIX_REPLACE_SHARE:
                    continue
                target = hiragana(run)
                for k, (piece, is_english) in enumerate(pieces):
                    at = piece.find(target) if not is_english else -1
                    if at >= 0:
                        pieces[k:k + 1] = [(piece[:at], False), (found[0], True), (piece[at + len(target):], False)]
                        text = text.replace(run, found[0], 1)
                        counts["put-in-english"] += 1
                        break
            mixed = any(is_english for _, is_english in pieces)
            typing = pick(MIX_TYPING) if mixed else "chunked"
            keys, japanese = "", []
            for piece, is_english in pieces:
                if not piece:
                    continue
                if is_english:
                    typed = piece if rng.random() < MIX_TYPED_CAPITALS else piece.lower()
                    if typing != "joined":
                        typed = ("" if not keys or keys.endswith(" ") else " ") + typed + " "
                else:
                    if typing == "chunked":
                        piece = "".join(c + (" " if c in PARTICLES and i + 1 < len(piece) and rng.random() < 0.6 else "")
                                        for i, c in enumerate(piece))
                    typed = type_reading(kana, piece)
                    if typed is None:
                        keys = None
                        break
                keys += typed
                japanese += [not is_english] * len(typed)
            if keys is None or not keys.strip():
                counts["untypable"] += 1
                continue
            keys = keys.rstrip()
            japanese = japanese[:len(keys)]
            if not mixed:
                # Japanese alone: some of it, and every line where a chunk
                # spells an English word too.
                contrast = any(chunk in words for chunk in keys.split(" "))
                if not contrast and rng.random() >= MIX_JAPANESE_ONLY:
                    continue
                counts["contrast" if contrast else "japanese-only"] += 1
            elif had:
                counts["had-english"] += 1
            if left_out(text):
                counts["dropped"] += 1
                continue
            kind = pick(MIX_SLIPS) if mixed else pick([("none", 0.6), ("japanese", 0.4)])
            if kind in ("english", "both"):
                spans = [m.span() for m in re.finditer(r"[A-Za-z]{2,}", keys)
                         if not any(japanese[i] for i in range(*m.span()))]
                if spans:
                    begin, end = rng.choice(spans)
                    typed = english_slip(keys[begin:end], words, rng)
                    if typed and typed.lower() != keys[begin:end].lower():
                        keys = keys[:begin] + typed.lower() + keys[end:]
                        japanese = japanese[:begin] + [False] * len(typed) + japanese[end:]
                        counts["english-slips"] += 1
            if kind in ("japanese", "both"):
                slipped = slip(keys, japanese, rng)
                if slipped:
                    keys = slipped
                    counts["japanese-slips"] += 1
            context = (row.get("left_context") or "").replace("\n", "")
            if rng.random() < NO_CONTEXT_SHARE:
                context = ""
            f.write(json.dumps({"prompt": prompt(context, keys), "text": text}, ensure_ascii=False) + "\n")
            counts["kept"] += 1
    print(counts)


SLIPS_PER_SENTENCE = [(0, 0.35), (1, 0.45), (2, 0.20)]
REAL_WORD_SHARE = 0.2  # of the slips, a real word typed for another


def english_slip(word: str, words: set[str], rng: random.Random) -> str | None:
    """`word` typed with one slip; None when it has too few letters."""
    lower = word.lower()
    if len(lower) < 2 or not lower.isalpha():
        return None
    if rng.random() < REAL_WORD_SHARE:
        near = set()
        for i in range(len(lower)):
            near.add(lower[:i] + lower[i + 1:])
            for c in neighbours(lower[i]):
                near.add(lower[:i] + c + lower[i + 1:])
            if i + 1 < len(lower):
                near.add(lower[:i] + lower[i + 1] + lower[i] + lower[i + 2:])
        near = sorted(w for w in near if w in words and w != lower)
        if near:
            typed = rng.choice(near)
            return typed.capitalize() if word[0].isupper() else typed
    i = rng.randrange(len(word))
    kind = rng.choice(["neighbour", "out", "doubled", "swapped"])
    if kind == "neighbour":
        near = neighbours(word[i].lower())
        if not near:
            return None
        c = rng.choice(near)
        return word[:i] + (c.upper() if word[i].isupper() else c) + word[i + 1:]
    if kind == "out":
        return word[:i] + word[i + 1:] if len(word) > 2 else None
    if kind == "doubled":
        return word[:i + 1] + word[i] + word[i + 1:]
    if i + 1 < len(word) and word[i] != word[i + 1]:
        return word[:i] + word[i + 1] + word[i] + word[i + 2:]
    return None


def prepare_english(args) -> None:
    import re
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    left_out = excluded_test(args)
    words = set()
    for line in open(args.lexicon, encoding="utf-8"):
        word = line.split("\t")[0].strip().lower()
        if word.isalpha():
            words.add(word)
    rng = random.Random(0)
    counts = {"kept": 0, "dropped": 0, "slips": 0}
    with open(out / "examples.jsonl", "w", encoding="utf-8", newline="\n") as f:
        for line in open(args.english, encoding="utf-8"):
            text = line.rstrip("\n").split("\t", 1)[-1].strip()
            if not text or len(text) > ENGLISH_MAX_CHARS or not text.isascii():
                continue
            if left_out(text):
                counts["dropped"] += 1
                continue
            pieces = re.split(r"([A-Za-z]+)", text)
            letters = [k for k, piece in enumerate(pieces) if piece.isalpha()]
            r, want = rng.random(), 0
            for n, share in SLIPS_PER_SENTENCE:
                if r < share:
                    want = n
                    break
                r -= share
            for k in rng.sample(letters, min(want, len(letters))):
                typed = english_slip(pieces[k], words, rng)
                if typed and typed != pieces[k]:
                    pieces[k] = typed
                    counts["slips"] += 1
            f.write(json.dumps({"prompt": INPUT_TAG + "".join(pieces) + OUTPUT_TAG, "text": text}) + "\n")
            counts["kept"] += 1
    print(counts)


def train(args) -> None:
    import numpy
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer

    out = Path(args.out)
    # The GPU also draws the screen: at most --gpu-share of its memory, and
    # a pause after each step so it is busy about that share of the time.
    torch.cuda.set_per_process_memory_fraction(args.gpu_share)
    tokenizer = AutoTokenizer.from_pretrained(args.base, trust_remote_code=True)
    model = AutoModelForCausalLM.from_pretrained(args.base, torch_dtype=torch.bfloat16, trust_remote_code=True).cuda()
    # Activations are worked out again in the backward pass rather than
    # kept: the memory goes to longer batches.
    model.gradient_checkpointing_enable()
    model.config.use_cache = False
    # --lora R: low-rank adapters on every linear layer, the weights frozen
    # (a model too large to train whole in the GPU's memory); merged in when
    # saved. The adapters sit in the model's own modules, so it is used as is.
    adapted = None
    if args.lora:
        from peft import LoraConfig, get_peft_model
        model.enable_input_require_grads()
        adapted = get_peft_model(model, LoraConfig(r=args.lora, lora_alpha=2 * args.lora, lora_dropout=0.0,
                                                   target_modules="all-linear"))
        adapted.print_trainable_parameters()
    model.train()
    eos = tokenizer.eos_token_id
    bos = tokenizer.bos_token_id

    pieces = {}

    def prompt_ids(text):
        if not args.per_char_reading:
            return tokenizer(text, add_special_tokens=False)["input_ids"]
        # --per-char-reading: the reading one character a token (each
        # character tokenized on its own), so that no token runs across the
        # reading's word boundaries; the context is tokenized as text.
        head, _, rest = text.rpartition(INPUT_TAG)
        reading = rest[:-len(OUTPUT_TAG)] if rest.endswith(OUTPUT_TAG) else rest
        ids = tokenizer(head + INPUT_TAG, add_special_tokens=False)["input_ids"]
        for c in reading + OUTPUT_TAG:
            if c not in pieces:
                pieces[c] = tokenizer(c, add_special_tokens=False)["input_ids"]
            ids += pieces[c]
        return ids

    def encode(row):
        p = prompt_ids(row["prompt"])
        t = tokenizer(row["text"], add_special_tokens=False)["input_ids"] + [eos]
        ids = ([bos] if bos is not None else []) + p + t
        # Held as int32 arrays: ten million examples as Python lists would
        # not fit in memory.
        return numpy.asarray(ids, dtype=numpy.int32), len(ids) - len(t)

    # An even sample of the examples, encoded as they are read so that
    # neither the file nor its rows are held.
    path = out / "examples.jsonl"
    with open(path, encoding="utf-8") as f:
        total = sum(1 for _ in f)
    share = min(1.0, (args.examples + 2000) * 1.02 / total)
    # --seed: another sample of the same examples (training on from a run).
    pick = random.Random(args.seed)
    with open(path, encoding="utf-8") as f:
        rows = [encode(json.loads(l)) for l in f if pick.random() < share]
    random.Random(args.seed).shuffle(rows)
    check, rows = rows[:2000], rows[2000:2000 + args.examples]

    def batches(encoded, tokens_per_batch, skip=0):
        order = sorted(range(len(encoded)), key=lambda i: len(encoded[i][0]))
        groups, group, longest = [], [], 0
        for i in order:
            longest = max(longest, len(encoded[i][0]))
            if group and longest * (len(group) + 1) > tokens_per_batch:
                groups.append(group)
                group, longest = [], len(encoded[i][0])
            group.append(i)
        if group:
            groups.append(group)
        random.Random(2).shuffle(groups)
        for g in groups[skip:]:
            width = max(len(encoded[i][0]) for i in g)
            ids = torch.full((len(g), width), eos, dtype=torch.long)
            labels = torch.full((len(g), width), -100, dtype=torch.long)
            mask = torch.zeros((len(g), width), dtype=torch.long)
            for row, i in enumerate(g):
                seq, start = encoded[i]
                ids[row, :len(seq)] = torch.tensor(seq)
                labels[row, start:len(seq)] = torch.tensor(seq[start:])
                mask[row, :len(seq)] = 1
            yield ids.cuda(), labels.cuda(), mask.cuda()

    def loss_of(ids, labels, mask):
        # The vocabulary's logits only where the text is learned: for every
        # position they would not fit in the GPU's memory.
        hidden = model.model(input_ids=ids, attention_mask=mask).last_hidden_state[:, :-1]
        targets = labels[:, 1:]
        keep = targets != -100
        logits = model.lm_head(hidden[keep]).float()
        return torch.nn.functional.cross_entropy(logits, targets[keep])

    def evaluate():
        model.eval()
        total, count = 0.0, 0
        with torch.no_grad():
            for ids, labels, mask in batches(check, args.batch_tokens):
                loss = loss_of(ids, labels, mask)
                total += loss.item()
                count += 1
        model.train()
        return total / max(count, 1)

    steps = sum(1 for _ in batches(rows, args.batch_tokens))
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=0.0, betas=(0.9, 0.95))
    warmup = max(1, steps // 50)
    schedule = torch.optim.lr_scheduler.LambdaLR(
        optimizer, lambda s: min(1.0, (s + 1) / warmup) * 0.5 * (1 + math.cos(math.pi * min(s, steps) / steps)))
    # --save-every: the trained weights and the optimizer are kept in
    # resume/ now and then, and a run started again with the same arguments
    # goes on from there (a stopped run is not lost).
    resume = out / "resume"
    trained = [(n, p) for n, p in model.named_parameters() if p.requires_grad]
    first = 0
    if (resume / "state.pt").exists():
        state = torch.load(resume / "state.pt", weights_only=False)
        with torch.no_grad():
            for n, p in trained:
                p.copy_(state["weights"][n])
        optimizer.load_state_dict(state["optimizer"])
        schedule.load_state_dict(state["schedule"])
        first = state["step"] + 1
        print(f"going on from step {first}", flush=True)
    else:
        print(f"{len(rows)} examples, {steps} steps; check loss {evaluate():.4f}", flush=True)
    start, seen = time.time(), 0
    for step, (ids, labels, mask) in enumerate(batches(rows, args.batch_tokens, first), first):
        began = time.time()
        loss = loss_of(ids, labels, mask)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        optimizer.step()
        schedule.step()
        optimizer.zero_grad(set_to_none=True)
        seen += int(mask.sum())
        torch.cuda.synchronize()
        time.sleep((time.time() - began) * (1 / args.gpu_share - 1))
        if step % 200 == 0:
            rate = seen / (time.time() - start)
            print(f"step {step}/{steps} loss {loss.item():.4f} {rate:.0f} tok/s", flush=True)
        if step and step % args.check_every == 0:
            print(f"  check loss {evaluate():.4f}", flush=True)
        if args.save_every and step and step % args.save_every == 0:
            resume.mkdir(exist_ok=True)
            torch.save({"step": step, "weights": {n: p.detach().cpu() for n, p in trained},
                        "optimizer": optimizer.state_dict(), "schedule": schedule.state_dict()},
                       resume / "state.tmp")
            os.replace(resume / "state.tmp", resume / "state.pt")
    print(f"done; check loss {evaluate():.4f}, {time.time() - start:.0f} s", flush=True)
    if adapted is not None:
        model = adapted.merge_and_unload()
    model.save_pretrained(out / "model", safe_serialization=True)
    tokenizer.save_pretrained(out / "model")


RANK_NEGATIVES = 4  # TEKITO's wrong sentences per example
RANK_LM_WEIGHT = 0.5  # how much the meant text's own loss counts beside the choice


def same_but_form(a: str, b: str) -> bool:
    """Whether two sentences differ only in width or punctuation forms."""
    import unicodedata
    marks = str.maketrans({"，": "、", ",": "、", "．": "。", "｡": "。", "､": "、"})
    return unicodedata.normalize("NFKC", a).translate(marks) == unicodedata.normalize("NFKC", b).translate(marks)


def prepare_rank(args) -> None:
    """Joins TEKITO's conversions of a set of sentences (tekito_ja_eval
    --write-sentences, and TEKITO_EVAL_OUTPUT for what it shows first) with
    what each meant: rank.jsonl, a prompt and the meant text first, then
    TEKITO's wrong sentences, most likely first."""
    out = Path(args.out)
    rng = random.Random(0)
    contexts = {}
    if args.context:
        for line in open(args.context, encoding="utf-8"):
            rid, _, context = line.rstrip("\n").partition("\t")
            contexts[rid] = context
    shown, ranked = {}, {}
    for path in sorted(Path(args.lists).glob("rank.out.*.tsv")):
        for line in open(path, encoding="utf-8"):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3:
                shown[parts[1]] = parts[2]
    for path in sorted(Path(args.lists).glob("rank.sentences.*.tsv")):
        for line in open(path, encoding="utf-8"):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 5 and parts[3] == "0":
                ranked.setdefault(parts[1], []).append(parts[4])
    counts = {"kept": 0, "no_negatives": 0, "tekito_right": 0}
    with open(out / "rank.jsonl", "w", encoding="utf-8", newline="\n") as f:
        for line in open(args.gen, encoding="utf-8"):
            rid, reading, meant = line.rstrip("\n").split("\t")[:3]
            negatives = []
            for text in [shown.get(rid, "")] + ranked.get(rid, []):
                if (text and text != meant and not same_but_form(text, meant) and
                        not any(same_but_form(text, other) for other in negatives)):
                    negatives.append(text)
                if len(negatives) == RANK_NEGATIVES:
                    break
            if not negatives:
                counts["no_negatives"] += 1
                continue
            counts["tekito_right"] += shown.get(rid) == meant
            context = contexts.get(rid, "") if rng.random() >= NO_CONTEXT_SHARE else ""
            f.write(json.dumps({"prompt": prompt(context, reading), "texts": [meant] + negatives},
                               ensure_ascii=False) + "\n")
            counts["kept"] += 1
        if args.rehearse:
            # Writing kept up: rows with the meant text alone add only its
            # own loss.
            with open(args.rehearse, encoding="utf-8") as source:
                total = sum(1 for _ in source)
            share = counts["kept"] / max(total, 1)
            pick = random.Random(3)
            counts["rehearsed"] = 0
            with open(args.rehearse, encoding="utf-8") as source:
                for line in source:
                    if pick.random() < share:
                        row = json.loads(line)
                        f.write(json.dumps({"prompt": row["prompt"], "texts": [row["text"]]},
                                           ensure_ascii=False) + "\n")
                        counts["rehearsed"] += 1
    print(counts)


def train_rank(args) -> None:
    """Teaches a model trained for conversion (train) to choose: for each
    example, the meant text against TEKITO's wrong sentences, each scored as
    ln p(text + EOS | prompt) as TekitoAI scores them, the choice a softmax
    over them, with the meant text's own loss beside it."""
    import numpy
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer

    out = Path(args.out)
    torch.cuda.set_per_process_memory_fraction(args.gpu_share)
    tokenizer = AutoTokenizer.from_pretrained(args.base, trust_remote_code=True)
    model = AutoModelForCausalLM.from_pretrained(args.base, torch_dtype=torch.bfloat16, trust_remote_code=True).cuda()
    model.gradient_checkpointing_enable()
    model.config.use_cache = False
    model.train()
    eos = tokenizer.eos_token_id
    bos = tokenizer.bos_token_id

    def encode(row):
        p = ([bos] if bos is not None else []) + tokenizer(row["prompt"], add_special_tokens=False)["input_ids"]
        seqs = []
        for text in row["texts"]:
            t = tokenizer(text, add_special_tokens=False)["input_ids"] + [eos]
            seqs.append((numpy.asarray(p + t, dtype=numpy.int32), len(p)))
        return seqs

    with open(out / "rank.jsonl", encoding="utf-8") as f:
        rows = [encode(json.loads(l)) for l in f]
    random.Random(1).shuffle(rows)
    check, rows = rows[:2000], rows[2000:]

    def batches(data, tokens_per_batch, shuffle=True):
        groups, group, used = [], [], 0
        for i, seqs in enumerate(data):
            size = len(seqs) * max(len(s) for s, _ in seqs)
            if group and used + size > tokens_per_batch:
                groups.append(group)
                group, used = [], 0
            group.append(i)
            used += size
        if group:
            groups.append(group)
        if shuffle:
            random.Random(2).shuffle(groups)
        for g in groups:
            seqs = [s for i in g for s in data[i]]
            sizes = [len(data[i]) for i in g]
            width = max(len(s) for s, _ in seqs)
            ids = torch.full((len(seqs), width), eos, dtype=torch.long)
            labels = torch.full((len(seqs), width), -100, dtype=torch.long)
            mask = torch.zeros((len(seqs), width), dtype=torch.long)
            for row, (seq, start) in enumerate(seqs):
                ids[row, :len(seq)] = torch.from_numpy(seq.astype(numpy.int64))
                labels[row, start:len(seq)] = torch.from_numpy(seq[start:].astype(numpy.int64))
                mask[row, :len(seq)] = 1
            yield ids.cuda(), labels.cuda(), mask.cuda(), sizes

    def scores_of(ids, labels, mask):
        hidden = model.model(input_ids=ids, attention_mask=mask).last_hidden_state[:, :-1]
        targets = labels[:, 1:]
        keep = targets != -100
        logits = model.lm_head(hidden[keep]).float()
        token = torch.log_softmax(logits, dim=-1).gather(1, targets[keep].unsqueeze(1)).squeeze(1)
        which = torch.arange(ids.shape[0], device=ids.device).unsqueeze(1).expand_as(targets)[keep]
        totals = torch.zeros(ids.shape[0], device=ids.device).index_add(0, which, token)
        lengths = torch.zeros(ids.shape[0], device=ids.device).index_add(0, which, torch.ones_like(token))
        return totals, lengths

    def loss_of(ids, labels, mask, sizes):
        totals, lengths = scores_of(ids, labels, mask)
        choice, own, right, at = 0.0, 0.0, 0, 0
        for size in sizes:
            scores = totals[at:at + size]
            choice = choice - torch.log_softmax(scores, dim=0)[0]
            own = own - scores[0] / lengths[at]
            right += int(torch.argmax(scores).item() == 0)
            at += size
        n = len(sizes)
        return choice / n + args.lm_weight * own / n, right, n

    def evaluate():
        model.eval()
        total, right, n = 0.0, 0, 0
        with torch.no_grad():
            for ids, labels, mask, sizes in batches(check, args.batch_tokens, shuffle=False):
                loss, r, k = loss_of(ids, labels, mask, sizes)
                total += loss.item() * k
                right += r
                n += k
        model.train()
        return total / max(n, 1), right / max(n, 1)

    steps = sum(1 for _ in batches(rows, args.batch_tokens))
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=0.0, betas=(0.9, 0.95))
    warmup = max(1, steps // 50)
    schedule = torch.optim.lr_scheduler.LambdaLR(
        optimizer, lambda s: min(1.0, (s + 1) / warmup) * 0.5 * (1 + math.cos(math.pi * min(s, steps) / steps)))
    loss, accuracy = evaluate()
    print(f"{len(rows)} examples, {steps} steps; check loss {loss:.4f} meant first {accuracy:.3f}", flush=True)
    start = time.time()
    for step, (ids, labels, mask, sizes) in enumerate(batches(rows, args.batch_tokens)):
        began = time.time()
        loss, _, _ = loss_of(ids, labels, mask, sizes)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        optimizer.step()
        schedule.step()
        optimizer.zero_grad(set_to_none=True)
        torch.cuda.synchronize()
        time.sleep((time.time() - began) * (1 / args.gpu_share - 1))
        if step % 200 == 0:
            print(f"step {step}/{steps} loss {loss.item():.4f}", flush=True)
        if step and step % args.check_every == 0:
            loss, accuracy = evaluate()
            print(f"  check loss {loss:.4f} meant first {accuracy:.3f}", flush=True)
    loss, accuracy = evaluate()
    print(f"done; check loss {loss:.4f} meant first {accuracy:.3f}, {time.time() - start:.0f} s", flush=True)
    model.save_pretrained(out / "model", safe_serialization=True)
    tokenizer.save_pretrained(out / "model")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("step", choices=["prepare", "prepare-keys", "prepare-english", "prepare-mix", "train", "prepare-rank",
                                         "train-rank"])
    parser.add_argument("--out", required=True)
    parser.add_argument("--zenz")
    parser.add_argument("--exclude-dir", action="append", default=[])
    parser.add_argument("--exclude-sha1", action="append", default=[])
    parser.add_argument("--every", type=int, default=1)
    parser.add_argument("--loanwords")
    parser.add_argument("--names", action="append",
                        help="prepare-mix: katakana TAB English name TAB language editions (Wikidata labels)")
    parser.add_argument("--english")
    parser.add_argument("--lexicon")
    parser.add_argument("--base")
    parser.add_argument("--examples", type=int, default=300000)
    parser.add_argument("--lr", type=float, default=5e-5)
    parser.add_argument("--batch-tokens", type=int, default=4096)
    parser.add_argument("--check-every", type=int, default=2000)
    parser.add_argument("--save-every", type=int, default=2000, help="train: steps between saves to go on from")
    parser.add_argument("--gpu-share", type=float, default=0.9)
    parser.add_argument("--seed", type=int, default=1, help="train: which sample of the examples")
    parser.add_argument("--per-char-reading", action="store_true",
                        help="train: the reading one character a token (a model to run with per-character readings)")
    parser.add_argument("--lora", type=int, default=0, help="train: low-rank adapters of this rank instead of all weights")
    parser.add_argument("--gen", help="prepare-rank: id, reading, meant text")
    parser.add_argument("--rehearse", type=Path,
                        help="prepare-rank: examples.jsonl of a train run; as many of them as there are rank "
                             "examples are put among them (the meant text only), so the model keeps writing")
    parser.add_argument("--lm-weight", type=float, default=RANK_LM_WEIGHT,
                        help="train-rank: how much the meant text's own loss counts beside the choice")
    parser.add_argument("--context", help="prepare-rank: id, the text before it")
    parser.add_argument("--lists", help="prepare-rank: the folder of rank.sentences.*.tsv and rank.out.*.tsv")
    args = parser.parse_args()
    {"prepare": prepare, "prepare-keys": prepare_keys, "prepare-english": prepare_english, "prepare-mix": prepare_mix,
     "train": train,
     "prepare-rank": prepare_rank, "train-rank": train_rank}[args.step](args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
