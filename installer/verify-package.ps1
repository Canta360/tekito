# Checks an extracted TEKITO package before installing it: every file listed
# in package-manifest.json is present and unchanged, the scripts parse, and
# each Data Pack matches its checksums. install.ps1 is only run after this
# passes.
[CmdletBinding()]
param(
    [string]$PackageRoot = $PSScriptRoot,
    [switch]$RequireSingleExeInstaller
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$PackageRoot = (Resolve-Path $PackageRoot).Path
$manifestPath = Join-Path $PackageRoot "package-manifest.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "package-manifest.json was not found in $PackageRoot."
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ([string]$manifest.product -ne "TEKITO") { throw "This is not a TEKITO package." }

function Resolve-ChildPath([string]$BasePath, [string]$RelativePath) {
    $base = [IO.Path]::GetFullPath($BasePath).TrimEnd('\') + '\'
    $target = [IO.Path]::GetFullPath((Join-Path $base $RelativePath))
    if (-not $target.StartsWith($base, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Manifest path escapes its package directory: $RelativePath"
    }
    return $target
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

if ($RequireSingleExeInstaller) {
    $installerPath = Join-Path (Split-Path $PackageRoot -Parent) ([string]$manifest.single_exe_installer)
    $hashPath = "$installerPath.sha256"
    if (-not (Test-Path -LiteralPath $installerPath) -or -not (Test-Path -LiteralPath $hashPath)) {
        throw "The single-file installer or its checksum is missing next to the package."
    }
    $expected = ((Get-Content -LiteralPath $hashPath -Raw).Trim() -split "\s+")[0].ToUpperInvariant()
    if ((Get-Sha256 $installerPath) -ne $expected) { throw "Single-EXE installer checksum mismatch: $installerPath" }
}

foreach ($entry in $manifest.files.PSObject.Properties) {
    $path = Resolve-ChildPath $PackageRoot ([string]$entry.Name).Replace('/', '\')
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing package file: $($entry.Name)" }
    if ((Get-Sha256 $path) -ne ([string]$entry.Value).ToUpperInvariant()) {
        throw "Package file was changed: $($entry.Name)"
    }
}
foreach ($required in @("TEKITO.exe", "Tekito.Tsf.dll", "settings-ui/index.html", "install.ps1",
        "scripts/install-data-packs.ps1", "TEKITO_LICENSE.md", "THIRD_PARTY_NOTICES.md")) {
    if (-not $manifest.files.PSObject.Properties[$required]) { throw "The package manifest does not list $required." }
}

foreach ($script in Get-ChildItem -LiteralPath $PackageRoot -Filter *.ps1 -File -Recurse) {
    $tokens = $null
    $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile($script.FullName, [ref]$tokens, [ref]$errors) | Out-Null
    if ($errors.Count -gt 0) { throw "PowerShell syntax error in $($script.FullName): $($errors[0].Message)" }
}

$packs = @(Get-ChildItem -LiteralPath (Join-Path $PackageRoot "data") -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName "manifest.json") })
if ($packs.Count -ne [int]$manifest.data_packs) {
    throw "The package should have $($manifest.data_packs) Data Packs but has $($packs.Count)."
}
foreach ($pack in $packs) {
    $packManifest = Get-Content (Join-Path $pack.FullName "manifest.json") -Raw | ConvertFrom-Json
    $dataPath = Resolve-ChildPath $pack.FullName ([string]$packManifest.file)
    $noticePath = Resolve-ChildPath $pack.FullName ([string]$packManifest.notice_file)
    if (-not (Test-Path $dataPath) -or -not (Test-Path $noticePath)) {
        throw "Data Pack $($pack.Name) is incomplete."
    }
    if ((Get-Sha256 $dataPath) -ne ([string]$packManifest.sha256.file).ToUpperInvariant()) {
        throw "Data Pack $($pack.Name) failed its checksum."
    }
    if ($packManifest.PSObject.Properties.Name -contains "index_file") {
        $indexPath = Resolve-ChildPath $pack.FullName ([string]$packManifest.index_file)
        if (-not (Test-Path $indexPath) -or
            (Get-Sha256 $indexPath) -ne ([string]$packManifest.sha256.index).ToUpperInvariant()) {
            throw "Data Pack $($pack.Name) failed its checksum."
        }
    }
}

Write-Host "The TEKITO package is complete: $(@($manifest.files.PSObject.Properties).Count) files and $($packs.Count) Data Packs verified."
