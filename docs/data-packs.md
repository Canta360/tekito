# Data Packs

Everything TEKITO knows about English and Japanese comes from data packs:
mostly sorted, tab-separated files with a small byte-offset index beside
them, and for Japanese conversion, binary files mapped into memory. TEKITO
reads only what a lookup needs and never fetches anything over the network
while it runs. The published installer carries the English packs; the
Japanese ones (`japanese-*` apart from `japanese-phonetic`) are a separate
download that setup fetches when Japanese is chosen.

| Pack | Contents | Source | License |
| --- | --- | --- | --- |
| `standard-english` | The spelling lexicon | SCOWL / ESDB, plus reviewed additions | SCOWL terms, CC BY-SA 4.0 for the additions |
| `wikipedia-common-misspellings` | Known misspelling → correction pairs | Wikipedia | CC BY-SA 4.0 |
| `frequency` | How common each word is | Leipzig Corpora, English news 2025 | CC BY 4.0 |
| `phrase` | Which words follow which | Leipzig Corpora, English news 2025 | CC BY 4.0 |
| `dictionary-display` | Short definitions and parts of speech | WordNet 3.0 | WordNet license |
| `proper-nouns` | Countries, regions, cities of 15,000+ people, continents and seas | GeoNames | CC BY 4.0 |
| `slang` | Chat abbreviations and informal spellings, kept as typed | Written for TEKITO (`scripts/en/slang-seeds.tsv`) | TEKITO (see `LICENSE.md`) |
| `wiktionary-slang` | More slang and abbreviations | Wiktionary via kaikki.org | CC BY-SA 4.0 |
| `pronunciation` | Pronunciations | CMU Pronouncing Dictionary | CMUdict license |
| `emoji` | Emoji and their names | Unicode CLDR | Unicode License v3 |
| `social-expression` | Emoji, emoticons, kaomoji, symbols and short forms for chat words | Written for TEKITO (`scripts/en/social-expression-seeds.tsv`) | TEKITO (see `LICENSE.md`) |
| `japanese-phonetic` | English words from how they sound in romaji ("konpyuutaa" → computer) | Generated from CMUdict; words chosen by Leipzig frequency | CMUdict license |
| `special-conversions` | Dates and times for words like "today" and "きょう", symbols by ASCII spelling ("->" → →) or Japanese reading ("やじるし" → →), Japanese emoticons ("にこにこ" → (^^)), single kanji by reading, and the kanji for numbers | Written for TEKITO (`scripts/build-special-conversions.py`); Japanese symbols, emoticons and single kanji from Mozc (`src/data/symbol`, `src/data/emoticon`, `src/data/single_kanji`) | TEKITO (see `LICENSE.md`), BSD-3-Clause |
| `qwerty-typo-catalog` | Synthetic typos of common words, for evaluation only | Generated for TEKITO | TEKITO (see `LICENSE.md`) |
| `japanese-romaji` | How typed keys become kana in Japanese input | Written for TEKITO (`scripts/ja/build-japanese-romaji.py`) | TEKITO (see `LICENSE.md`) |
| `japanese-core` | Words, readings and how they join, for kana-kanji conversion | Mozc OSS dictionary (IPAdic, Okinawa dictionary) plus TEKITO slang entries | IPAdic license, BSD-3-Clause, TEKITO source license |
| `japanese-lm` | Which words go together, for conversion to prefer them (neighbor pairs, and content words of one sentence) | Leipzig Corpora Collection, Japanese news crawl 2019 and Wikipedia 2021 | CC BY 4.0 |
| `japanese-loanwords` | English words for katakana candidates (ミーティング → meeting) | Generated from japanese-phonetic (CMUdict) with Leipzig frequencies | CMUdict license, CC BY 4.0 |
| `japanese-slang` | Short definitions for curated Japanese slang; these meanings take priority for matching headwords | Written for TEKITO (`scripts/ja/japanese-slang-seeds.tsv`) | TEKITO (see `LICENSE.md`) |
| `japanese-wiktionary` | What Japanese words mean, shown beside the highlighted candidate | Japanese Wiktionary via kaikki.org | CC BY-SA 4.0 (kept as its own pack) |
| `japanese-wordnet` | Meanings of the words Wiktionary lacks | Japanese WordNet 1.1 (NICT) | Japanese WordNet license |
| `japanese-zipcode` | The addresses postal codes cover (1000001 → 東京都千代田区千代田); not shipped, added by the user in Settings | Japan Post postal code data | No copyright claimed by Japan Post |

Each pack directory holds its data files, any lookup index, a `manifest.json`
with the version, source, license and SHA-256 checksums for its files, and a
`NOTICE` with the attribution its license asks for. The repository keeps them
by language, so English and Japanese can be worked on apart: `data\en`,
`data\ja`, and `data\common` for `special-conversions`, which both use.
TEKITO installs them side by side in `%LOCALAPPDATA%\TEKITO\data`, and looks
for each pack there or in `TEKITO_DATA_PACK_DIR` when that is set, either side
by side or in those language folders.

## Getting the data

`phrase` (about 380 MB) is too large for the repository. Rebuild it, and
any other downloaded pack, from the public sources:

```powershell
.\scripts\en\prepare-full-data-packs.ps1
```

It downloads the sources into `.cache\tekito-data`, runs
`build-full-data-packs.py` and checks the result. TEKITO works without
`phrase`; it just loses some context ranking.

`slang`, `social-expression`, `japanese-phonetic`, `japanese-loanwords` and
`qwerty-typo-catalog` are generated by `scripts\en\build-expression-packs.py` from the seed files and
the pronunciation, frequency and standard-english packs:

```powershell
python scripts\en\build-expression-packs.py
```

`japanese-core` (about 34 MB) is also built locally. It is two binary files
that TEKITO maps into memory instead of the usual text and index:

```powershell
.\scripts\ja\prepare-japanese-packs.ps1
```

It downloads the Mozc dictionary at a pinned commit into `.cache\tekito-data`
and runs `build-japanese-packs.py`, which adds the curated terms from
`scripts\ja\japanese-slang-seeds.tsv`. Without it, Japanese input types kana
but cannot convert to kanji. The pack also carries `pos.tsv`, the part of
speech each kind of word a user adds in Settings takes, and the one digits
typed in Japanese take so counters join them (3こ -> 3個); a pack built before
it converts as before but leaves the user's Japanese words out. `japanese-romaji` is generated by
`scripts\ja\build-japanese-romaji.py`.

`special-conversions` is one small text file (`rules.tsv`, no index) read
whole at startup. `scripts\build-special-conversions.py` writes it from the
dates, formats, names and English symbols in the script and Mozc's symbol,
emoticon and single-kanji tables at a pinned commit:

```powershell
python scripts\build-special-conversions.py --mozc-symbols <symbol.tsv> --mozc-emoticons <emoticon.tsv> --mozc-single-kanji <single_kanji.tsv> --mozc-license <LICENSE> --mozc-commit <sha>
```

The same script builds `japanese-lm` (about 43 MB) with
`build-japanese-lm.py`, from two Leipzig Japanese corpora (about 530 MB to
download): word counts, how much likelier neighbor pairs are than chance,
and which content words share sentences. Without it, conversion goes by
the dictionary's parts of speech alone.

It also builds the small `japanese-slang` meaning pack from
`scripts\ja\japanese-slang-seeds.tsv`; its definitions take priority for matching
terms, then Wiktionary and WordNet provide fallback meanings. The script then
builds `japanese-wiktionary`
and `japanese-wordnet` (about 13 MB and 10 MB) with
`build-japanese-meaning-packs.py`, which
downloads the kaikki.org Japanese Wiktionary extract and the Japanese
WordNet. Each is one TSV sorted by the word (`surface`, hiragana `reading`
or empty, then the senses) that TEKITO searches in place. Without those
downloaded sources, curated slang meanings remain available and other
candidates fall back to English meanings (from `dictionary-display`).

`japanese-zipcode` (about 5 MB) is not shipped. Japan Post updates its
postal code data monthly, so users download `utf_ken_all.zip` from
https://www.post.japanpost.jp/zipcode/dl/utf-zip.html and drop it on
Settings (Dictionary & learning), which builds the
pack in the data folder (`src/UserData/PostalCodeImport.cpp`). Without it,
postal codes convert as numbers only.

## Changing a pack

After editing a pack, update its `manifest.json` checksums and `NOTICE`, then
run the validator, which checks the index against the data, the checksums,
duplicate keys and score ranges:

```powershell
python scripts\validate-data-packs.py
```
