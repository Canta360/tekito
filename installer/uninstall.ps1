# Removes TEKITO: unregisters the input method, takes it out of the keyboard
# list and deletes the installed program, its Start menu shortcut and its
# entry in Installed apps. The user's dictionary, learning, settings and Data
# Packs stay unless -RemoveUserData is given; -ExportTo <folder> first saves
# the user's words there (the Japanese words as Microsoft IME's word list,
# the English words as TEKITO's own file).
#
# Apps that type with TEKITO keep its files open until they close: what
# cannot be deleted now is deleted at the next sign-in.
# Exit code 0: removed; 3: removed, some files go at the next sign-in;
# 30: the words could not be exported, and nothing was removed.
[CmdletBinding()]
param(
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA "Programs\TEKITO"),
    [switch]$SkipRegistration,
    [switch]$RemoveUserData,
    [string]$ExportTo
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$clsid = "{6F67E5C8-A873-4B69-8EC3-26DF00F642F1}"
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\TEKITO"
$cleanupValue = "TEKITO cleanup"

# Apps that take text input keep the input method DLL loaded, so it cannot
# be deleted while they run. It can be renamed, though: move such files out
# of the way and let the next install, uninstall or sign-in remove them.
function Remove-InstalledFiles([string]$Root) {
    if (-not (Test-Path -LiteralPath $Root)) { return }
    foreach ($file in @(Get-ChildItem -LiteralPath $Root -File -Recurse -Force)) {
        try {
            Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
        } catch {
            try {
                Rename-Item -LiteralPath $file.FullName ("{0}.old-{1}" -f $file.Name, [guid]::NewGuid().ToString("N")) -ErrorAction Stop
            } catch { }
        }
    }
    Get-ChildItem -LiteralPath $Root -Directory -Recurse -Force | Sort-Object { $_.FullName.Length } -Descending |
        Where-Object { @(Get-ChildItem -LiteralPath $_.FullName -Force).Count -eq 0 } |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
}

$installRoot = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')
$userRoot = [IO.Path]::GetFullPath($env:USERPROFILE).TrimEnd('\') + '\'
if (-not $installRoot.StartsWith($userRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "InstallRoot must be a folder inside your user profile."
}
if ((Test-Path -LiteralPath $installRoot) -and
    -not (Test-Path -LiteralPath (Join-Path $installRoot "install-manifest.json")) -and
    @(Get-ChildItem -LiteralPath $installRoot -Filter "Tekito.Tsf*.dll" -File -Recurse -ErrorAction SilentlyContinue).Count -eq 0) {
    throw "$installRoot does not look like a TEKITO installation."
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $SkipRegistration -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an administrator PowerShell to unregister the input method."
}

Get-Process -Name TEKITO -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

# The user's words first: when they cannot be saved, nothing is removed.
if ($ExportTo) {
    $app = Join-Path $installRoot "TEKITO.exe"
    $userData = Join-Path $env:LOCALAPPDATA "TEKITO\user.db"
    if (Test-Path -LiteralPath $userData) {
        if (-not (Test-Path -LiteralPath $app)) {
            Write-Host "TEKITO.exe is missing, so the words could not be exported."
            exit 30
        }
        $process = Start-Process $app -ArgumentList @('--export-dictionaries', ('"{0}"' -f $ExportTo)) -Wait -PassThru
        if ($process.ExitCode -ne 0) {
            Write-Host "The words could not be exported to $ExportTo."
            exit 30
        }
    }
}

if (-not $SkipRegistration) {
    $regsvr32 = Join-Path $env:WINDIR "System32\regsvr32.exe"
    foreach ($dll in @(Get-ChildItem -LiteralPath $installRoot -Filter "Tekito.Tsf*.dll" -File -Recurse -ErrorAction SilentlyContinue)) {
        $process = Start-Process $regsvr32 -ArgumentList @('/u', '/s', $dll.FullName) -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -ne 0) { Write-Warning "Unregistering $($dll.Name) returned $($process.ExitCode)." }
    }

    # TEKITO for English and TEKITO for Japanese.
    $tips = @("0409:$clsid{8D99240A-5C9C-4ED8-8DE4-85F21DF23763}",
              "0411:$clsid{2876747C-FA52-4CCC-B308-F406190F8CDE}")
    $languages = Get-WinUserLanguageList
    $changed = $false
    foreach ($language in $languages) {
        foreach ($tip in $tips) {
            if ($language.InputMethodTips -contains $tip) {
                [void]$language.InputMethodTips.Remove($tip)
                $changed = $true
            }
        }
    }
    if ($changed) { Set-WinUserLanguageList $languages -Force }
    # What Windows kept of the input method for this user.
    Remove-Item -LiteralPath "HKCU:\Software\Microsoft\CTF\TIP\$clsid" -Recurse -Force -ErrorAction SilentlyContinue
}

$startMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\TEKITO"
if (Test-Path -LiteralPath $startMenu) { Remove-Item -LiteralPath $startMenu -Recurse -Force }
Remove-InstalledFiles $installRoot
$userDataRoot = Join-Path $env:LOCALAPPDATA "TEKITO"
if ($RemoveUserData) { Remove-InstalledFiles $userDataRoot }
Remove-Item -LiteralPath $uninstallKey -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path ([IO.Path]::GetTempPath()) "TEKITO-install.log") -Force -ErrorAction SilentlyContinue

# Files still open: deleted at the next sign-in.
$left = @(@($installRoot) + $(if ($RemoveUserData) { @($userDataRoot) } else { @() }) |
    Where-Object { Test-Path -LiteralPath $_ })
$runOnce = "HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce"
if ($left.Count -gt 0) {
    $command = 'cmd.exe /d /c ' + (($left | ForEach-Object { 'rd /s /q "{0}"' -f $_ }) -join ' & ')
    New-Item -Path $runOnce -Force | Out-Null
    Set-ItemProperty -LiteralPath $runOnce -Name $cleanupValue -Value $command
    Write-Host "TEKITO was removed. Some files are still in use; they are deleted the next time you sign in."
    exit 3
}
Remove-ItemProperty -LiteralPath $runOnce -Name $cleanupValue -ErrorAction SilentlyContinue
Write-Host "TEKITO was removed.$(if ($RemoveUserData) { ' Your dictionary, learning and settings were deleted too.' } else { ' Your dictionary, learning and settings were kept.' })"
exit 0
