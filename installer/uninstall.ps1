# Removes TEKITO: unregisters the input method, takes it out of the keyboard
# list and deletes the installed program. The user's dictionary, learning,
# settings and Data Packs stay unless -RemoveUserData is given.
[CmdletBinding()]
param(
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA "Programs\TEKITO"),
    [switch]$SkipRegistration,
    [switch]$RemoveUserData
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

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

if (-not $SkipRegistration) {
    $regsvr32 = Join-Path $env:WINDIR "System32\regsvr32.exe"
    foreach ($dll in @(Get-ChildItem -LiteralPath $installRoot -Filter "Tekito.Tsf*.dll" -File -Recurse -ErrorAction SilentlyContinue)) {
        $process = Start-Process $regsvr32 -ArgumentList @('/u', '/s', $dll.FullName) -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -ne 0) { Write-Warning "Unregistering $($dll.Name) returned $($process.ExitCode)." }
    }

    $tip = "0409:{6F67E5C8-A873-4B69-8EC3-26DF00F642F1}{8D99240A-5C9C-4ED8-8DE4-85F21DF23763}"
    $languages = Get-WinUserLanguageList
    $changed = $false
    foreach ($language in $languages) {
        if ($language.InputMethodTips -contains $tip) {
            [void]$language.InputMethodTips.Remove($tip)
            $changed = $true
        }
    }
    if ($changed) { Set-WinUserLanguageList $languages -Force }
}

$startMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\TEKITO"
if (Test-Path -LiteralPath $startMenu) { Remove-Item -LiteralPath $startMenu -Recurse -Force }
if (Test-Path -LiteralPath $installRoot) { Remove-Item -LiteralPath $installRoot -Recurse -Force }
if ($RemoveUserData) {
    $userDataRoot = Join-Path $env:LOCALAPPDATA "TEKITO"
    if (Test-Path -LiteralPath $userDataRoot) { Remove-Item -LiteralPath $userDataRoot -Recurse -Force }
}

Write-Host "TEKITO was removed.$(if ($RemoveUserData) { ' Your dictionary, learning and settings were deleted too.' } else { ' Your dictionary, learning and settings were kept.' })"
