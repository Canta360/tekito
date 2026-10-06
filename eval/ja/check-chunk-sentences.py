"""Checks sentences typed in chunks and writes them as a keys file for
`tekito_ja_eval --chunks`.

Input: JSON lines, one sentence each:

  {"category": "ja" | "mixed" | "en",
   "chunks": [{"keys": "kyouha", "lang": "ja", "kana": "きょうは"},
              {"keys": "github", "lang": "en"}, ...],
   "expected": ["今日はGitHubに…", …]}

A Japanese chunk is kept only when its keys, read with the japanese-romaji
table the way TEKITO reads them, make exactly its kana; a sentence with a
chunk that does not is left out (and counted). English chunks must be
lower-case letters, with . , ! ? after them at most.

  python eval/ja/check-chunk-sentences.py sentences.jsonl out_keys.tsv
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "en"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))


def load_romaji():
    import importlib.util
    path = Path(__file__).resolve().parents[2] / "scripts" / "en" / "build-expression-packs.py"
    spec = importlib.util.spec_from_file_location("expression_packs", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.Romaji()


def hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def main() -> int:
    source, out = Path(sys.argv[1]), Path(sys.argv[2])
    romaji = load_romaji()
    marks = {".": "。", ",": "、", "!": "！", "?": "？", "-": "ー"}
    kept, dropped, reasons = [], 0, {}
    for number, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        try:
            row = json.loads(line)
            keys = []
            for chunk in row["chunks"]:
                typed = chunk["keys"].strip()
                if chunk["lang"] == "en":
                    if not re.fullmatch(r"[a-z]+[.,!?]*", typed):
                        raise ValueError(f"english keys {typed!r}")
                    keys.append(typed + "*")
                    continue
                if not re.fullmatch(r"[a-z\-.,!?]+", typed):
                    raise ValueError(f"japanese keys {typed!r}")
                letters = re.sub(r"[.,!?]+$", "", typed)
                tail = "".join(marks[c] for c in typed[len(letters):])
                kana = romaji.kana(letters) if letters else ""
                if kana is None or kana + tail != hiragana(chunk["kana"].strip()):
                    raise ValueError(f"{typed!r} reads {kana!r}, not {chunk['kana']!r}")
                keys.append(typed)
            expected = [text.strip() for text in row["expected"] if text.strip()]
            if not expected:
                raise ValueError("no expected text")
            kept.append("\t".join([f"fresh-{row['category']}", str(number), " ".join(keys), *expected]))
        except (KeyError, ValueError, TypeError, json.JSONDecodeError) as error:
            dropped += 1
            reason = str(error).split(" ")[0]
            reasons[reason] = reasons.get(reason, 0) + 1
            print(f"  left out {number}: {error}", file=sys.stderr)
    out.write_text("\n".join(kept) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(kept)} sentences kept, {dropped} left out -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
