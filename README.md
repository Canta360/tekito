# TEKITO

English typing help for Windows keyboards. TEKITO fixes typos when you press
Space, offers the word you were going for, and gets out of the way when you
meant what you typed. It is an input method (a TSF text service), so it works
in ordinary Windows apps without plugins, and it runs entirely on your PC.

[日本語](README_JP.md)

![The TEKITO candidate list: glass in light and dark, and the simple style](docs/images/candidate-list.png)

> TEKITO is a preview. It has been tested in Notepad; other apps are listed in
> [the compatibility notes](docs/compatibility.md).

## How it works

Type as usual. The word you are typing is underlined and a short list of
choices appears under it.

| Key | What it does |
| --- | --- |
| Space | Applies a confident correction and adds the space. Press again to step through the choices. |
| Shift+Space | Goes back to the previous choice. |
| Tab, ↑, ↓ | Moves through the list without committing. |
| Backspace | Right after a correction, puts back what you typed. |
| Esc | Keeps what you typed. |
| Enter | Picks the highlighted choice when you are in the list; otherwise ends the line as usual. |
| Alt+` | Switches between Auto and Direct. Direct types exactly the keys you press. |

Typing the next letter settles the word, so there is nothing to undo later.
TEKITO leaves alone words it recognizes, names, anything that looks like
code, URLs and addresses, and fields marked as passwords, numbers or email.
Terminals, and any apps you add in Settings, get no help at all.

![TEKITO Settings](docs/images/settings.png)

## Privacy

Everything TEKITO does happens on your computer. It does not send your
typing anywhere, it has no account and no telemetry, and it does not use the
network while you type. What it learns from your choices stays in
`%LOCALAPPDATA%\TEKITO` and can be cleared from Settings. See
[the privacy notice](PRIVACY.md).

## Install

Download the installer from the releases page and run it. It needs the
Microsoft Edge WebView2 Runtime, which Windows 11 already has. After
installing, sign out and back in, then pick TEKITO with Win+Space.

## Build

You need Windows 11, Visual Studio 2026 (or 2022) with the C++
desktop workload, CMake 3.24 or later, and Node.js for the Settings page.

```powershell
cd apps\tekito-settings\ui
npm ci
npm run build
cd ..\..\..
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
.\scripts\verify.ps1
```

`verify.ps1` builds everything, runs the tests and checks the rules in
[Working on TEKITO](#working-on-tekito). To try your build, register it from
an administrator PowerShell with `scripts\register-debug.ps1`
and remove it again with `unregister-debug.ps1`.

The language data lives in `data/`. The two largest packs (place names and
phrase statistics) are too big for the repository;
`scripts\prepare-full-data-packs.ps1` downloads the public
sources and rebuilds them. See [Data Packs](docs/data-packs.md).

To make an installer, build the `windows-x64-release` preset and run
`installer\package-release.ps1`.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/Core` | Candidate search, ranking and the typing state machine. No Windows code. |
| `src/Tsf` | The TSF text service and the candidate window. |
| `src/UserData` | Settings, the user dictionary and learning, stored in SQLite. |
| `tests` | Unit tests for the engine and user data. |
| `apps/tekito-settings` | The Settings window: a Win32 host and a React page in WebView2. |
| `installer` | The setup program and packaging scripts. |
| `data` | Language data packs, each with its own manifest and NOTICE. |

## Working on TEKITO

- Words and phrases belong in data packs, never in `if` statements.
- Nothing in the input path may use the network, and diagnostics never
  include what the user typed.
- TEKITO works through TSF only; no keyboard hooks or synthesized input.
- Leave correct words, names and code as they are. When unsure, suggest
  instead of replacing.

Bug reports and ideas are welcome in Issues. We are not taking pull requests
while TEKITO is in preview.

## License

The source code is available under the
[Functional Source License 1.1](LICENSE.md) (FSL-1.1-ALv2): use it, change
it and share it for anything except a competing commercial product. Each
release becomes Apache 2.0 two years after it is published. The installer we
publish is covered by [the TEKITO License Agreement](installer/TEKITO_LICENSE.md),
and each data pack keeps the license of its source. The details are in
[Licensing](LICENSING.md) and
[third-party notices](THIRD_PARTY_NOTICES.md).

TEKITO is made by [Capitata](https://capitata.dev).
