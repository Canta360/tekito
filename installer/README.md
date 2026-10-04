# Installer

`package-release.ps1` turns a Release build into an offline package, a
single-file installer and the Japanese data:

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
.\installer\package-release.ps1 -ValidateOnly   # check the inputs
.\installer\package-release.ps1
```

It writes to `artifacts\`:

- `TEKITO-<version>-full\`, the package: `TEKITO.exe`, `Tekito.Tsf.dll`, the
  Settings page, the English Data Packs, the install scripts and the license
  texts, with `package-manifest.json` holding the SHA-256 of every file.
- `TEKITO-<version>-full.zip`, the same package zipped.
- `TEKITO-<version>-full-installer.exe`, the setup program with the ZIP
  appended.
- `TEKITO-<version>-japanese-data.zip`, the Japanese Data Packs, too large to
  carry in the installer. Attach it to the GitHub release `v<version>`: the
  package manifest records its URL and SHA-256, and the installer downloads
  it from there when Japanese is chosen. `-JapaneseDataBaseUrl` changes where
  it is expected.

The Japanese packs must be built first (`scripts\ja\prepare-japanese-packs.ps1`).

The setup program (`TekitoSetup.cpp`) checks for the WebView2 Runtime, shows
the license agreement and the Japanese choice, then unpacks the ZIP to a
temporary folder, runs `verify-package.ps1` and `install.ps1`. To look at its
screens without installing anything, build the `tekito_setup_preview` target.

The scripts can also be run by hand from an administrator PowerShell in the
extracted package:

| Script | What it does |
| --- | --- |
| `verify-package.ps1` | Checks every file against the package manifest. |
| `install.ps1` | Installs to `%LOCALAPPDATA%\Programs\TEKITO`, installs the Data Packs to `%LOCALAPPDATA%\TEKITO\data`, registers the input method and adds a Start menu shortcut. |
| `uninstall.ps1` | Removes the program and both keyboard list entries. `-RemoveUserData` also deletes the dictionary, learning, settings and Data Packs. |
| `scripts\verify-installed.ps1` | Read-only check of an installation. |

`install.ps1` options for Japanese:

| Option | Effect |
| --- | --- |
| `-Japanese $true` | Downloads the Japanese data, checks its SHA-256, installs it and adds TEKITO to the Japanese keyboard list in place of the English one. |
| `-JapaneseDataPath <zip>` | The same from a ZIP already downloaded, for installing without a network. It is checked the same way. |
| `-KeepEnglishProfile $true` | Keeps TEKITO in the English keyboard list too. |

It exits with 20 when the Japanese data cannot be downloaded and 21 when the
file does not match, before changing anything.

The package and installer are not signed yet, so SmartScreen and Smart App
Control may block them.
