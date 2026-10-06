"""Fetches the opening sentence of Japanese Wikipedia articles titled in
Latin letters, where the katakana reading is usually given
("GitHub（ギットハブ）は、…"), for build-japanese-english-pack.py.

Reads jawiki/latin-titles.json (page id -> [title, language editions], made
by build-japanese-english-pack.py --list-latin) and appends to
jawiki/intros.jsonl, so an interrupted run goes on where it stopped.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from datapacks import ROOT  # noqa: E402

API = "https://ja.wikipedia.org/w/api.php"
AGENT = "TEKITO-data-builder/1.0 (https://github.com/Canta360/tekito)"
BATCH = 20


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--cache", type=Path, default=ROOT / ".cache" / "tekito-data")
    args = parser.parse_args()
    folder = args.cache / "jawiki"
    titles = json.loads((folder / "latin-titles.json").read_text(encoding="utf-8"))
    out = folder / "intros.jsonl"
    done = set()
    if out.exists():
        for line in out.read_text(encoding="utf-8").splitlines():
            done.add(json.loads(line)["id"])
    todo = [page_id for page_id in titles if int(page_id) not in done]
    print(f"{len(done):,} fetched, {len(todo):,} to go")
    with out.open("a", encoding="utf-8") as output:
        for start in range(0, len(todo), BATCH):
            batch = todo[start:start + BATCH]
            query = urllib.parse.urlencode({
                "action": "query", "format": "json", "formatversion": 2, "prop": "extracts",
                "exintro": 1, "explaintext": 1, "exsentences": 1, "exlimit": BATCH,
                "pageids": "|".join(batch), "maxlag": 5,
            })
            request = urllib.request.Request(f"{API}?{query}", headers={"User-Agent": AGENT})
            for attempt in range(5):
                try:
                    with urllib.request.urlopen(request, timeout=60) as response:
                        data = json.load(response)
                    break
                except Exception as error:  # network hiccup, or maxlag
                    print(f"retry after {error}")
                    time.sleep(10 * (attempt + 1))
            else:
                print("giving up for now; run again to go on")
                return 1
            for page in data.get("query", {}).get("pages", []):
                output.write(json.dumps({"id": page.get("pageid"), "title": page.get("title"),
                                         "text": page.get("extract", "")}, ensure_ascii=False) + "\n")
            output.flush()
            if start // BATCH % 50 == 0:
                print(f"{start + len(batch):,} / {len(todo):,}")
            time.sleep(0.5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
