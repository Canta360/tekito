"""Builds evaluation sets from text kept outside the repository, as JSON
lines for eval/ja/kana-to-keys.py (text, chunks, also):

  python eval/ja/external/make-sets.py chat DIR OUT_DIR --me NAME [--me NAME]... [--drop-word WORD]...
      LINE (.txt) and WhatsApp (.txt) exports in DIR: only the lines the
      user wrote (--me: their names in the exports), without stickers,
      photos, URLs, code, digits, emoji, names (--drop-word) and very short
      or long lines; mixed.jsonl, ja.jsonl, en.jsonl.
  python eval/ja/external/make-sets.py jecs DIR OUT_DIR
      JECS (Japanese-English code-switching corpus; the text is CC BY 3.0):
      DIR holds neutral/, joy/, sad/, anger/ with transcripts_{cs,ja}.txt;
      cs.jsonl (Japanese with English, for Mix) and ja.jsonl.
  python eval/ja/external/make-sets.py cpjd DIR OUT_DIR
      CPJD (dialect corpus, CC BY-SA 4.0): common.jsonl (the standard
      Japanese sentences) and dialect.jsonl.

Then, for each:  python eval/ja/kana-to-keys.py OUT_DIR/x.jsonl OUT_DIR/x_keys.tsv NAME
Mix evaluates the chunked keys (tekito_ja_eval --chunks ... --english-engine
--hold); Japanese mode the keys with the spaces taken out (--keys-corpus).

A chunk is each English word (@word) and each run of Japanese between them,
read with SudachiPy (pip install sudachipy sudachidict_core). A right text is
the line as written, and the same with no space beside Japanese
(Windows 11で, LAN ケーブル). None of these texts belongs in the repository:
a chat is the user's own; CPJD (and the NU multi-party chat corpus, also CC
BY-SA 4.0) is for evaluation only, never TEKITO's data or a model's
training; JECS may be kept with its attribution.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from sudachipy import dictionary

TOKENIZER = dictionary.Dictionary().create()
ALLOWED = re.compile(r"^[ぁ-ゖァ-ヺー一-鿿A-Za-z 、。！？!?,.]+$")
JAPANESE = re.compile(r"[ぁ-ゖァ-ヺー一-鿿]")
LATIN = re.compile(r"[A-Za-z]{2,}")
MARKS = {"、": "、", "。": "。", "！": "！", "？": "？", "!": "！", "?": "？", ",": "、", ".": "。"}


def hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def without_spaces_by_japanese(text: str) -> str:
    return re.sub(r"(?<=[ぁ-ー一-鿿]) +| +(?=[ぁ-ー一-鿿])", "", text)


def chunks_of(text: str) -> list[str] | None:
    out: list[str] = []
    run = ""
    for m in TOKENIZER.tokenize(text):
        s = m.surface()
        if re.fullmatch(r"[A-Za-z]+", s):
            if run:
                out.append(run)
                run = ""
            out.append("@" + s.lower())
        elif not s.strip():
            if run:
                out.append(run)
                run = ""
        elif s in MARKS:
            if out and not run and out[-1].startswith("@"):
                out[-1] += {"、": ",", "。": ".", "！": "!", "？": "?"}[MARKS[s]]
            else:
                run += MARKS[s]
        elif JAPANESE.search(s):
            r = m.reading_form()
            if not r or r == "キゴウ":
                return None
            run += hiragana(r)
        else:
            return None
    if run:
        out.append(run)
    return out


def row(text: str) -> str | None:
    text = text.strip()
    if not ALLOWED.match(text):
        return None
    chunks = chunks_of(text)
    if not chunks:
        return None
    also = [t for t in {without_spaces_by_japanese(text)} if t != text]
    return json.dumps({"text": text, "chunks": chunks, "also": also}, ensure_ascii=False)


def write(path: Path, rows: list[str]) -> None:
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")
    print(f"{path.name}: {len(rows)}")


# --- chats

LINE_MESSAGE = re.compile(r"^(\d{1,2}:\d{2})\t([^\t]*)\t(.*)$")
LINE_DATE = re.compile(r"^\d{4}/\d{1,2}/\d{1,2}\(.\)$")
WHATSAPP_MESSAGE = re.compile(r"^(\d{4}/\d{2}/\d{2} \d{1,2}:\d{2}) - ([^:]+?): (.*)$")
DROP = re.compile(r"^\[(スタンプ|写真|動画|ファイル|アルバム|ボイスメッセージ|位置情報|連絡先|ノート|イベント|投票)\]|^☎|通話|"
                  r"メッセージの送信を取り消しました|<添付ファイル>|<メディアなし>|このメッセージは削除されました|^\[.*\]$")
ADDRESS = re.compile(r"https?://|www\.|\.com\b|\.jp\b|@\w")


def messages(path: Path):
    """(sender, text) of a LINE or WhatsApp export."""
    text = path.read_text(encoding="utf-8-sig", errors="replace").replace("‎", "")
    current = None
    for raw in text.splitlines():
        m = LINE_MESSAGE.match(raw) or WHATSAPP_MESSAGE.match(raw)
        if m:
            if current:
                yield current
            body = m.group(3).strip()
            if body.startswith('"') and body.endswith('"'):
                body = body[1:-1].replace('""', '"')
            current = [m.group(2).strip(), body]
        elif LINE_DATE.match(raw) or raw.startswith(("[LINE]", "保存日時")) or re.match(r"^\d{4}/\d{2}/\d{2} \d{1,2}:\d{2} - ", raw):
            if current:
                yield current
            current = None
        elif current is not None:
            current[1] += "\n" + raw
    if current:
        yield current


def chat(args) -> None:
    sets: dict[str, list[str]] = {"mixed": [], "ja": [], "en": []}
    seen = set()
    for path in sorted(Path(args.dir).rglob("*.txt")):
        for sender, body in messages(path):
            if sender not in args.me:
                continue
            for line in body.splitlines():
                line = line.strip()
                if (len(line) < 5 or len(line) > 100 or DROP.search(line) or ADDRESS.search(line) or
                        any(word in line for word in args.drop_word) or len(re.findall(r"\d", line)) > 4 or line in seen):
                    continue
                seen.add(line)
                ja, en = bool(JAPANESE.search(line)), bool(LATIN.search(line))
                kind = "mixed" if ja and en else "ja" if ja else "en" if en else None
                if kind and (r := row(line)):
                    sets[kind].append(r)
    for kind, rows in sets.items():
        write(Path(args.out) / f"{kind}.jsonl", rows)


def jecs(args) -> None:
    for kind in ("cs", "ja"):
        rows, seen = [], set()
        for emotion in ("neutral", "joy", "sad", "anger"):
            path = Path(args.dir) / emotion / f"transcripts_{kind}.txt"
            if not path.exists():
                continue
            for line in path.read_text(encoding="utf-8-sig").splitlines():
                if ":" not in line:
                    continue
                text = line.split(":", 1)[1].strip().replace("*", "")
                if text not in seen and (r := row(text)):
                    seen.add(text)
                    rows.append(r)
        write(Path(args.out) / f"{kind}.jsonl", rows)


def cpjd(args) -> None:
    root = Path(args.dir)
    for name, files in (("common", sorted(root.glob("common_set*.txt"))),
                        ("dialect", sorted(root.glob("[FM]0*/transcript.txt")))):
        rows, seen = [], set()
        for path in files:
            for line in path.read_text(encoding="utf-8-sig").splitlines():
                text = line.split(":", 1)[1].strip() if ":" in line else ""
                if text and text not in seen and (r := row(text)):
                    seen.add(text)
                    rows.append(r)
        write(Path(args.out) / f"{name}.jsonl", rows)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="kind", required=True)
    c = sub.add_parser("chat")
    c.add_argument("dir")
    c.add_argument("out")
    c.add_argument("--me", action="append", required=True)
    c.add_argument("--drop-word", action="append", default=[])
    for name in ("jecs", "cpjd"):
        p = sub.add_parser(name)
        p.add_argument("dir")
        p.add_argument("out")
    args = parser.parse_args()
    Path(args.out).mkdir(parents=True, exist_ok=True)
    {"chat": chat, "jecs": jecs, "cpjd": cpjd}[args.kind](args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
