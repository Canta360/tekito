# Contributing to TEKITO

[日本語](CONTRIBUTING_JP.md)

## Reports and ideas

Issues are welcome. Each kind has its own form:

- **Wrong conversion**: what you typed, what TEKITO gave and what you meant.
  Settings → About → **Report wrong conversions** fills it in for you.
- **Bug**: anything else TEKITO did wrong. Settings → About → **Report a
  problem** adds the version and settings.
- **Feature request**: something you would like TEKITO to do.

Issues are public. Remove anything private before you submit. What you
report may be used to improve TEKITO, for example as a test sentence.

## Pull requests

We are not taking pull requests while TEKITO is in preview.

## Building and checking

See [Build](README.md#build). Run `scripts\verify.ps1` before every commit:
it builds TEKITO, runs the tests and checks the rules below.

## Rules

- Words and phrases belong in data packs, never in `if` statements.
- Nothing in the input path may use the network, and diagnostics never
  include what the user typed.
- TEKITO works through TSF only: no keyboard hooks. Synthesized input is used
  only to put the caret back between brackets in older edit controls.
- Leave correct words, names and code as they are. When unsure, suggest
  instead of replacing.
- Data must allow commercial use. Each data pack names its source and
  license in its `NOTICE` and `manifest.json`.

## License

The source code is under the
[Functional Source License 1.1](LICENSE.md) (FSL-1.1-ALv2). See
[Licensing](LICENSING.md).
