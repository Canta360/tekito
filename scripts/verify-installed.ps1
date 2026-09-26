# Read-only check of an installed or registered TEKITO: the input method
# registration, the TEKITO.exe beside it, and the installed Data Packs.
# Exits with 1 when something is wrong.
param(
    [string]$DllPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$clsid = "{6F67E5C8-A873-4B69-8EC3-26DF00F642F1}"
$tip = "0409:$clsid{8D99240A-5C9C-4ED8-8DE4-85F21DF23763}"

$registrationPath = "Registry::HKEY_CLASSES_ROOT\CLSID\$clsid\InprocServer32"
$registeredDll = if (Test-Path $registrationPath) {
    (Get-ItemProperty -LiteralPath $registrationPath).'(default)'
} else { $null }
if (-not $DllPath) { $DllPath = $registeredDll }
if (-not $DllPath -or -not (Test-Path -LiteralPath $DllPath)) {
    throw "No TEKITO input method DLL is registered. Pass -DllPath to check a specific build."
}
$resolvedDll = (Resolve-Path $DllPath).Path
$appPath = Join-Path (Split-Path $resolvedDll -Parent) "TEKITO.exe"

$registrationMatches = $registeredDll -and
    [string]::Equals((Resolve-Path $registeredDll).Path, $resolvedDll, [StringComparison]::OrdinalIgnoreCase)
$tipPresent = @(Get-WinUserLanguageList | Where-Object { $_.InputMethodTips -contains $tip }).Count -gt 0

$dataRoot = if ($env:TEKITO_DATA_PACK_DIR) { $env:TEKITO_DATA_PACK_DIR } else {
    Join-Path $env:LOCALAPPDATA "TEKITO\data"
}
# TEKITO needs these; the rest (place names, phrase statistics, ...) add to
# it when installed.
$requiredPacks = @("standard-english", "wikipedia-common-misspellings", "frequency",
                   "dictionary-display", "slang", "wiktionary-slang", "pronunciation", "emoji")

function Test-Pack([string]$PackPath) {
    $manifest = Get-Content -LiteralPath (Join-Path $PackPath "manifest.json") -Raw | ConvertFrom-Json
    foreach ($field in @("file", "index_file", "notice_file")) {
        $path = [IO.Path]::GetFullPath((Join-Path $PackPath ([string]$manifest.$field)))
        if (-not $path.StartsWith([IO.Path]::GetFullPath($PackPath), [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $path)) { return $false }
    }
    $data = Join-Path $PackPath ([string]$manifest.file)
    $index = Join-Path $PackPath ([string]$manifest.index_file)
    return (Get-FileHash -LiteralPath $data -Algorithm SHA256).Hash -eq ([string]$manifest.sha256.file).ToUpperInvariant() -and
           (Get-FileHash -LiteralPath $index -Algorithm SHA256).Hash -eq ([string]$manifest.sha256.index).ToUpperInvariant()
}

$problems = @()
Write-Output "Input method DLL: $resolvedDll"
Write-Output "Registered: $registrationMatches"
if (-not $registrationMatches) { $problems += "the DLL is not the registered one ($registeredDll)" }
Write-Output "In the keyboard list: $tipPresent"
if (-not $tipPresent) { $problems += "TEKITO is not in the English keyboard list" }
Write-Output "TEKITO.exe: $(Test-Path -LiteralPath $appPath)"
if (-not (Test-Path -LiteralPath $appPath)) { $problems += "TEKITO.exe is missing next to the DLL" }

Write-Output "Data Packs in $dataRoot"
$installed = @(Get-ChildItem -LiteralPath $dataRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "manifest.json") })
foreach ($pack in $installed) {
    $valid = Test-Pack $pack.FullName
    Write-Output "  $($pack.Name): $(if ($valid) { 'ok' } else { 'damaged' })"
    if (-not $valid) { $problems += "pack $($pack.Name) failed its checksum" }
}
foreach ($pack in $requiredPacks) {
    if ($installed.Name -notcontains $pack) {
        Write-Output "  ${pack}: missing"
        $problems += "required pack $pack is missing"
    }
}

if ($problems.Count -gt 0) {
    Write-Output ""
    $problems | ForEach-Object { Write-Output "Problem: $_" }
    exit 1
}
Write-Output ""
Write-Output "TEKITO is installed correctly. Pick it with Win+Space; Alt+`` switches Auto and Direct."
exit 0
