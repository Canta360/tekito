# Installs TEKITO from an extracted package for the current user:
#   %LOCALAPPDATA%\Programs\TEKITO   TEKITO.exe, Tekito.Tsf.dll, settings-ui
#   %LOCALAPPDATA%\TEKITO\data       Data Packs
# registers the input method, adds it to the English keyboard list and makes
# a Start menu shortcut to Settings. Needs an administrator PowerShell unless
# -SkipRegistration is given. The user's dictionary, learning and settings in
# %LOCALAPPDATA%\TEKITO are never touched.
#
# -Japanese adds Japanese input: it downloads the Japanese Data Packs named in
# package-manifest.json (or takes them from -JapaneseDataPath), checks their
# SHA-256, and adds TEKITO to the Japanese keyboard list in place of the
# English one; TEKITO for Japanese has Auto and Direct for English. Pass
# -KeepEnglishProfile to keep both. Exit code 20: the download failed;
# 21: the downloaded file did not match.
[CmdletBinding()]
param(
    [string]$SourceRoot = $PSScriptRoot,
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA "Programs\TEKITO"),
    [bool]$StartMenuShortcut = $true,
    [bool]$Japanese = $false,
    [string]$JapaneseDataPath,
    [bool]$KeepEnglishProfile = $false,
    [switch]$SkipDataPacks,
    [switch]$SkipRegistration,
    [switch]$SkipPrerequisiteCheck
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$tip = "0409:{6F67E5C8-A873-4B69-8EC3-26DF00F642F1}{8D99240A-5C9C-4ED8-8DE4-85F21DF23763}"
$japaneseTip = "0411:{6F67E5C8-A873-4B69-8EC3-26DF00F642F1}{2876747C-FA52-4CCC-B308-F406190F8CDE}"
$regsvr32 = Join-Path $env:WINDIR "System32\regsvr32.exe"
$startMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\TEKITO"

function Invoke-Regsvr32([string[]]$Arguments) {
    $process = Start-Process $regsvr32 -ArgumentList $Arguments -WindowStyle Hidden -Wait -PassThru
    return $process.ExitCode
}

# Apps that take text input keep the input method DLL loaded, so it cannot
# be deleted while they run. It can be renamed, though: move such files out
# of the way and let the next install or uninstall remove them.
function Grant-AppContainerRead([string]$Folder) {
    # Store apps, Settings and the Start menu's search run in an app
    # container; the input method loads in them and needs to read the
    # program and its Data Packs (never user.db, which they cannot open).
    if (-not (Test-Path -LiteralPath $Folder)) { return }
    foreach ($sid in @("*S-1-15-2-1", "*S-1-15-2-2")) {
        & icacls.exe $Folder /grant "${sid}:(OI)(CI)(RX)" /Q | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Warning "Could not let apps read $Folder (icacls returned $LASTEXITCODE)." }
    }
}

function Remove-InstalledFiles([string]$Root) {
    if (-not (Test-Path -LiteralPath $Root)) { return }
    foreach ($file in @(Get-ChildItem -LiteralPath $Root -File -Recurse -Force)) {
        try {
            Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
        } catch {
            Rename-Item -LiteralPath $file.FullName ("{0}.old-{1}" -f $file.Name, [guid]::NewGuid().ToString("N"))
        }
    }
    Get-ChildItem -LiteralPath $Root -Directory -Recurse -Force | Sort-Object { $_.FullName.Length } -Descending |
        Where-Object { @(Get-ChildItem -LiteralPath $_.FullName -Force).Count -eq 0 } |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
}

function Remove-InputMethodTip([string[]]$Tips) {
    $languages = Get-WinUserLanguageList
    $changed = $false
    foreach ($language in $languages) {
        foreach ($oneTip in $Tips) {
            if ($language.InputMethodTips -contains $oneTip) {
                [void]$language.InputMethodTips.Remove($oneTip)
                $changed = $true
            }
        }
    }
    if ($changed) { Set-WinUserLanguageList $languages -Force }
}

# Puts TEKITO in a language's keyboard list, adding the language if needed.
# Returns whether it changed anything.
function Add-InputMethodTip($Languages, [string]$LanguageTag, [string]$Tip) {
    $language = $Languages | Where-Object { $_.LanguageTag -eq $LanguageTag } | Select-Object -First 1
    if (-not $language) {
        $language = (New-WinUserLanguageList $LanguageTag)[0]
        $Languages.Add($language)
    }
    if ($language.InputMethodTips -contains $Tip) { return $false }
    $language.InputMethodTips.Add($Tip)
    return $true
}

# The Japanese Data Packs, downloaded (or copied) and checked, unpacked into
# a new folder. Exits with 20 or 21 when they cannot be had.
function Get-JapaneseData($Manifest, [string]$LocalZip) {
    $info = if ($Manifest -and ($Manifest.PSObject.Properties.Name -contains "japanese_data")) { $Manifest.japanese_data } else { $null }
    if (-not $info -and -not $LocalZip) { throw "This package does not say where its Japanese data is." }
    $root = Join-Path ([IO.Path]::GetTempPath()) ("tekito-japanese-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $zip = Join-Path $root "japanese-data.zip"
    try {
        if ($LocalZip) {
            Copy-Item -LiteralPath $LocalZip $zip
        } else {
            Write-Host "Downloading $($info.url)"
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor
                [Net.SecurityProtocolType]::Tls12
            $ProgressPreference = "SilentlyContinue"
            Invoke-WebRequest -Uri ([string]$info.url) -OutFile $zip -UseBasicParsing
        }
    } catch {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "The Japanese data could not be downloaded: $($_.Exception.Message)"
        exit 20
    }
    if ($info -and (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToUpperInvariant() -ne
        ([string]$info.sha256).ToUpperInvariant()) {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "The Japanese data does not match this package."
        exit 21
    }
    $packs = Join-Path $root "packs"
    Expand-Archive -LiteralPath $zip -DestinationPath $packs -Force
    return $packs
}

# What the install did, for when it stops: %TEMP%\TEKITO-install.log.
$log = Join-Path ([IO.Path]::GetTempPath()) "TEKITO-install.log"
try { Start-Transcript -LiteralPath $log -Force | Out-Null } catch { }

$SourceRoot = (Resolve-Path $SourceRoot).Path
$packageManifestPath = Join-Path $SourceRoot "package-manifest.json"
$packageManifest = if (Test-Path -LiteralPath $packageManifestPath) {
    Get-Content -LiteralPath $packageManifestPath -Raw | ConvertFrom-Json
} else { $null }
if (-not $SkipPrerequisiteCheck) { & (Join-Path $SourceRoot "check-prerequisites.ps1") }

$sourceFiles = @("TEKITO.exe", "Tekito.Tsf.dll", "settings-ui\index.html") | ForEach-Object { Join-Path $SourceRoot $_ }
foreach ($file in $sourceFiles) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "The package is incomplete: $file is missing." }
}

$installRoot = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')
$userRoot = [IO.Path]::GetFullPath($env:USERPROFILE).TrimEnd('\') + '\'
if (-not $installRoot.StartsWith($userRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "InstallRoot must be a folder inside your user profile."
}
# An install that stopped half way leaves the program without its manifest;
# it is still TEKITO's folder.
$hadExistingInstall = (Test-Path -LiteralPath (Join-Path $installRoot "install-manifest.json") -PathType Leaf) -or
    (Test-Path -LiteralPath (Join-Path $installRoot "TEKITO.exe") -PathType Leaf)
if ((Test-Path -LiteralPath $installRoot) -and -not $hadExistingInstall -and
    @(Get-ChildItem -LiteralPath $installRoot -Force).Count -gt 0) {
    throw "InstallRoot must be empty or an existing TEKITO installation."
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $SkipRegistration -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an administrator PowerShell, or pass -SkipRegistration for a copy that is not registered."
}

# Fetch the Japanese data before anything changes, so a failed download
# leaves the current installation as it was.
$japaneseData = $null
if ($JapaneseDataPath) { $Japanese = $true }
if ($Japanese -and -not $SkipDataPacks) {
    $japaneseData = Get-JapaneseData $packageManifest $JapaneseDataPath
}

# Stage the new files first so a failure leaves the old installation alone.
$stagingRoot = Join-Path ([IO.Path]::GetTempPath()) ("tekito-install-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
$installedDll = Join-Path $installRoot "Tekito.Tsf.dll"
$installedApp = Join-Path $installRoot "TEKITO.exe"
$addedTips = @()
$registeredNewDll = $false
try {
    Copy-Item -LiteralPath (Join-Path $SourceRoot "TEKITO.exe") $stagingRoot
    Copy-Item -LiteralPath (Join-Path $SourceRoot "Tekito.Tsf.dll") $stagingRoot
    Copy-Item -LiteralPath (Join-Path $SourceRoot "settings-ui") $stagingRoot -Recurse

    # Data Packs first: if one cannot be replaced, the installed program is
    # left as it was.
    if (-not $SkipDataPacks) {
        & (Join-Path $SourceRoot "scripts\install-data-packs.ps1") `
            -SourceRoot (Join-Path $SourceRoot "data") `
            -DestinationRoot (Join-Path $env:LOCALAPPDATA "TEKITO\data")
        if ($japaneseData) {
            & (Join-Path $SourceRoot "scripts\install-data-packs.ps1") `
                -SourceRoot $japaneseData `
                -DestinationRoot (Join-Path $env:LOCALAPPDATA "TEKITO\data")
        }
    }

    Get-Process -Name TEKITO -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    if (-not $SkipRegistration) {
        # Older versions kept the DLL in a subfolder; unregister wherever it is.
        foreach ($oldDll in @(Get-ChildItem -LiteralPath $installRoot -Filter "Tekito.Tsf*.dll" -File -Recurse -ErrorAction SilentlyContinue)) {
            $code = Invoke-Regsvr32 @('/u', '/s', $oldDll.FullName)
            if ($code -ne 0) { Write-Warning "Unregistering the previous version returned $code." }
        }
    }
    Remove-InstalledFiles $installRoot
    New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
    Copy-Item -Path (Join-Path $stagingRoot "*") -Destination $installRoot -Recurse -Force
    Grant-AppContainerRead $installRoot
    Grant-AppContainerRead (Join-Path $env:LOCALAPPDATA "TEKITO\data")

    if (-not $SkipRegistration) {
        $code = Invoke-Regsvr32 @('/s', $installedDll)
        if ($code -ne 0) { throw "Registering the input method failed (regsvr32 returned $code)." }
        $registeredNewDll = $true

        # With Japanese, one TEKITO is enough: the Japanese one also types
        # English in Auto and Direct.
        $languages = Get-WinUserLanguageList
        $changed = $false
        if ($Japanese) {
            if (Add-InputMethodTip $languages "ja-JP" $japaneseTip) { $addedTips += $japaneseTip; $changed = $true }
        }
        if (-not $Japanese -or $KeepEnglishProfile) {
            if (Add-InputMethodTip $languages "en-US" $tip) { $addedTips += $tip; $changed = $true }
        } else {
            foreach ($language in $languages) {
                if ($language.InputMethodTips -contains $tip) {
                    [void]$language.InputMethodTips.Remove($tip)
                    $changed = $true
                }
            }
        }
        if ($changed) { Set-WinUserLanguageList $languages -Force }
    }

    $shortcutPath = Join-Path $startMenu "TEKITO Settings.lnk"
    if ($StartMenuShortcut) {
        New-Item -ItemType Directory -Path $startMenu -Force | Out-Null
        $shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcutPath)
        $shortcut.TargetPath = $installedApp
        $shortcut.Arguments = "--settings"
        $shortcut.WorkingDirectory = $installRoot
        $shortcut.IconLocation = "$installedApp,0"
        $shortcut.Save()
    } else {
        Remove-Item -LiteralPath $shortcutPath -Force -ErrorAction SilentlyContinue
    }

    [ordered]@{
        product = "TEKITO"
        version = if ($packageManifest) { [string]$packageManifest.version } else { "unknown" }
        installed_at = [DateTime]::UtcNow.ToString("o")
        install_root = $installRoot
        tsf_dll = $installedDll
        data_root = Join-Path $env:LOCALAPPDATA "TEKITO\data"
        start_menu_shortcut = $StartMenuShortcut
        japanese = $Japanese
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $installRoot "install-manifest.json") -Encoding UTF8

    Write-Host "TEKITO is installed in $installRoot."
    Write-Host "Sign out and back in, or restart your apps, then pick TEKITO with Win+Space."
}
catch {
    Write-Host "The install stopped: $($_.Exception.Message)"
    # Undo a first-time install that did not finish.
    if (-not $hadExistingInstall -and (Test-Path -LiteralPath $installRoot) -and
        -not (Test-Path -LiteralPath (Join-Path $installRoot "install-manifest.json"))) {
        if ($addedTips.Count -gt 0) { Remove-InputMethodTip $addedTips }
        if ($registeredNewDll) { [void](Invoke-Regsvr32 @('/u', '/s', $installedDll)) }
        Remove-Item -LiteralPath $startMenu -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $installRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    throw
}
finally {
    if ($stagingRoot -and (Test-Path -LiteralPath $stagingRoot)) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
    if ($japaneseData) {
        Remove-Item -LiteralPath (Split-Path $japaneseData -Parent) -Recurse -Force -ErrorAction SilentlyContinue
    }
    try { Stop-Transcript | Out-Null } catch { }
}
exit 0
