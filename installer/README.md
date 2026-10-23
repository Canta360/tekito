# Installer

`package-release.ps1` turns a Release build into the offline package, both
installers and the Japanese data:

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
- `TEKITO-<version>-full-installer.exe`, the offline setup program with the
  ZIP appended (English included; Japanese downloaded when chosen).
- `TEKITO-online-installer.exe`, a small stub that downloads the latest
  full installer from GitHub Releases, checks its SHA-256, then runs it.
  It can pass `/enable-updates` so setup turns on optional update checks.
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

The install also copies the setup program without its package
(`TekitoUninstall.exe`) and `uninstall.ps1` into the installation, and lists
TEKITO in Installed apps. Uninstalling there runs `TekitoUninstall.exe
/uninstall`: it offers to export the user's words to Documents, then removes
everything TEKITO put on the PC.

The scripts can also be run by hand from an administrator PowerShell in the
extracted package:

| Script | What it does |
| --- | --- |
| `verify-package.ps1` | Checks every file against the package manifest. |
| `install.ps1` | Installs to `%LOCALAPPDATA%\Programs\TEKITO`, installs the Data Packs to `%LOCALAPPDATA%\TEKITO\data`, registers the input method and adds a Start menu shortcut. |
| `uninstall.ps1` | Removes the program, both keyboard list entries, the Start menu shortcut and the Installed apps entry. `-RemoveUserData` also deletes the dictionary, learning, settings and Data Packs; `-ExportTo <folder>` first saves the user's words there. Files still in use are deleted at the next sign-in. |
| `scripts\verify-installed.ps1` | Read-only check of an installation. |

`install.ps1` options for Japanese and updates:

| Option | Effect |
| --- | --- |
| `-Japanese $true` | Downloads the Japanese data, checks its SHA-256, installs it and adds TEKITO to the Japanese keyboard list in place of the English one. |
| `-JapaneseDataPath <zip>` | The same from a ZIP already downloaded, for installing without a network. It is checked the same way. |
| `-KeepEnglishProfile $true` | Keeps TEKITO in the English keyboard list too. |
| `-CheckForUpdates $true` | Leaves a flag so Settings turns on optional GitHub update checks (typing never uses the network). |

It exits with 20 when the Japanese data cannot be downloaded and 21 when the
file does not match, before changing anything.

The package and installer are not signed yet, so SmartScreen and Smart App
Control may block them.
