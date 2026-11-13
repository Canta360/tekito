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
Report security problems privately, as in [Security](SECURITY.md).

## Pull requests

Pull requests are welcome.

1. For anything larger than a fix, open an Issue first, so that we can agree
   on the change before you write it.
2. One topic per pull request.
3. Sign the [Contributor License Agreement](CLA.md): the CLA check asks for
   it on your first pull request.
4. `scripts\verify.ps1` passes, and new behaviour has a test.
5. A change to conversion or correction shows its effect: the scores on the
   evaluation sets before and after (`eval/`), and sentences that changed.
   Measure on sentences you did not tune on.
6. Behaviour other input methods lack gets an on/off switch in Settings,
   under More settings, with a sensible default.
7. Settings, the setup program and messages are in English and Japanese;
   follow the wording of the existing text.

Code, comments and commit messages are in English. Issues and pull request
discussion may be in English or Japanese.

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

## Data

- Only data whose license allows commercial use.
- Each pack names its source and license in its `NOTICE` and
  `manifest.json`, and the script that builds it is in `scripts/`.
- No copying of other input methods' dictionaries.

## Code of Conduct

See [Code of Conduct](CODE_OF_CONDUCT.md).

## License

The source code is under the
[Functional Source License 1.1](LICENSE.md) (FSL-1.1-ALv2). See
[Licensing](LICENSING.md).
