# Standard English Data Pack

This pack is an offline, spelling-only American English wordlist derived
from the ESDB/SCOWL generated `en_US` size 60 wordlist, release 2026.02.25.
It also contains validated correction targets from the local Wikipedia common
misspellings pack when frequency, pronunciation, or WordNet independently
corroborates the target. Thirty additional targets were individually reviewed
against the local English Wiktionary dump; `cellpadding` was also checked
against the WHATWG HTML Standard. It is normalized to one lowercase ASCII word
per line and indexed by `ExternalLexiconProvider` for exact, prefix, and
bounded candidate lookup.

Definitions, parts of speech, pronunciation, frequency, phrase context,
proper nouns, slang, and misspelling mappings are separate providers/data
packs and are not stored here.

Source generation reference:

```text
scowl --db scowl.db word-list 60 A 1 --deaccent > lexicon.txt
```

See `NOTICE` and `manifest.json` for source, license, and checksums.
