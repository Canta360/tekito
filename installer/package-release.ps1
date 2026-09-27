# Builds the offline installer from a Release build:
#   artifacts\TEKITO-<version>-full\            the package, ready to zip
#   artifacts\TEKITO-<version>-full.zip          the package as a ZIP
#   artifacts\TEKITO-<version>-full-installer.exe  setup program + ZIP in one file
# -ValidateOnly checks the inputs without writing anything.
[CmdletBinding()]
param(
    [string]$BuildRoot,
    [string]$SourceDataRoot,
    [string]$OutputRoot,
    [string]$ArchivePath,
    [switch]$ValidateOnly
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
# The version comes from project(TEKITO VERSION x.y.z) in CMakeLists.txt.
$version = [regex]::Match((Get-Content (Join-Path $repoRoot "CMakeLists.txt") -Raw),
    'project\(TEKITO VERSION ([0-9.]+)').Groups[1].Value
if (-not $version) { throw "The version was not found in CMakeLists.txt." }
if (-not $OutputRoot) { $OutputRoot = Join-Path $repoRoot "artifacts\TEKITO-$version-full" }
if (-not $ArchivePath) { $ArchivePath = Join-Path $repoRoot "artifacts\TEKITO-$version-full.zip" }
if (-not $BuildRoot) { $BuildRoot = Join-Path $repoRoot "out\build\windows-x64-release" }
if (-not $SourceDataRoot) { $SourceDataRoot = Join-Path $repoRoot "data" }
$releaseRoot = Join-Path (Resolve-Path $BuildRoot).Path "Release"
$SourceDataRoot = (Resolve-Path $SourceDataRoot).Path

# Packs TEKITO cannot do without; the others are included when present.
$requiredPacks = @("standard-english", "wikipedia-common-misspellings", "frequency",
                   "dictionary-display", "slang", "wiktionary-slang", "pronunciation", "emoji")

function Get-RelativeUnixPath([string]$BasePath, [string]$TargetPath) {
    $base = (Resolve-Path -LiteralPath $BasePath).Path.TrimEnd('\') + '\'
    $target = (Resolve-Path -LiteralPath $TargetPath).Path
    return $target.Substring($base.Length).Replace('\', '/')
}

function Resolve-ChildPath([string]$BasePath, [string]$RelativePath) {
    $base = [IO.Path]::GetFullPath($BasePath).TrimEnd('\') + '\'
    $target = [IO.Path]::GetFullPath((Join-Path $base $RelativePath))
    if (-not $target.StartsWith($base, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Manifest path escapes its Data Pack directory: $RelativePath"
    }
    return $target
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

# Files the package carries, as package path -> source path.
$binaries = [ordered]@{
    "TEKITO.exe" = Join-Path $releaseRoot "TEKITO.exe"
    "Tekito.Tsf.dll" = Join-Path $releaseRoot "Tekito.Tsf.dll"
}
$setupStub = Join-Path $releaseRoot "TekitoSetupStub.exe"
$documents = [ordered]@{
    "README.md" = Join-Path $repoRoot "README.md"
    "README_JP.md" = Join-Path $repoRoot "README_JP.md"
    "INSTALL_JP.md" = Join-Path $PSScriptRoot "INSTALL_JP.md"
    "TEKITO_LICENSE.md" = Join-Path $PSScriptRoot "TEKITO_LICENSE.md"
    "LICENSING.md" = Join-Path $repoRoot "LICENSING.md"
    "PRIVACY.md" = Join-Path $repoRoot "PRIVACY.md"
    "THIRD_PARTY_NOTICES.md" = Join-Path $repoRoot "THIRD_PARTY_NOTICES.md"
    "docs\compatibility.md" = Join-Path $repoRoot "docs\compatibility.md"
    "assets\fonts\OFL.txt" = Join-Path $repoRoot "assets\fonts\OFL.txt"
    "third_party\webview2\LICENSE.txt" = Join-Path $repoRoot "third_party\webview2\LICENSE.txt"
    "third_party\webview2\NOTICE.txt" = Join-Path $repoRoot "third_party\webview2\NOTICE.txt"
}
$scripts = [ordered]@{
    "install.ps1" = Join-Path $PSScriptRoot "install.ps1"
    "uninstall.ps1" = Join-Path $PSScriptRoot "uninstall.ps1"
    "verify-package.ps1" = Join-Path $PSScriptRoot "verify-package.ps1"
    "check-prerequisites.ps1" = Join-Path $PSScriptRoot "check-prerequisites.ps1"
    "scripts\install-data-packs.ps1" = Join-Path $repoRoot "scripts\install-data-packs.ps1"
    "scripts\verify-installed.ps1" = Join-Path $repoRoot "scripts\verify-installed.ps1"
}
foreach ($source in @($binaries.Values) + @($documents.Values) + @($scripts.Values) + @($setupStub)) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing input: $source" }
}
if (-not (Test-Path -LiteralPath (Join-Path $releaseRoot "settings-ui\index.html"))) {
    throw "The Settings page was not built. Run npm run build in apps\tekito-settings\ui, then rebuild."
}

$packs = @(Get-ChildItem -LiteralPath $SourceDataRoot -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName "manifest.json") })
foreach ($required in $requiredPacks) {
    if ($packs.Name -notcontains $required) { throw "Required Data Pack is missing: $required" }
}
foreach ($pack in $packs) {
    $manifest = Get-Content (Join-Path $pack.FullName "manifest.json") -Raw | ConvertFrom-Json
    foreach ($field in @("file", "index_file", "notice_file", "sha256")) {
        if ($null -eq $manifest.$field) { throw "Pack $($pack.Name) is missing manifest field $field." }
    }
    $dataPath = Resolve-ChildPath $pack.FullName ([string]$manifest.file)
    $indexPath = Resolve-ChildPath $pack.FullName ([string]$manifest.index_file)
    $noticePath = Resolve-ChildPath $pack.FullName ([string]$manifest.notice_file)
    if (-not (Test-Path $dataPath) -or -not (Test-Path $indexPath) -or -not (Test-Path $noticePath)) {
        throw "Pack $($pack.Name) is missing its data, index or NOTICE."
    }
    if ((Get-Sha256 $dataPath) -ne ([string]$manifest.sha256.file).ToUpperInvariant() -or
        (Get-Sha256 $indexPath) -ne ([string]$manifest.sha256.index).ToUpperInvariant()) {
        throw "Pack $($pack.Name) failed checksum validation."
    }
}
if ($ValidateOnly) {
    Write-Host "Inputs are complete: $($binaries.Count) binaries, the setup program, the Settings page and $($packs.Count) Data Packs."
    exit 0
}

$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$artifactRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "artifacts")).TrimEnd('\') + '\'
if (-not $OutputRoot.StartsWith($artifactRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputRoot must be under $artifactRoot."
}
if (Test-Path -LiteralPath $OutputRoot) { Remove-Item -LiteralPath $OutputRoot -Recurse -Force }
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null

foreach ($entry in @($binaries.GetEnumerator()) + @($documents.GetEnumerator()) + @($scripts.GetEnumerator())) {
    $target = Join-Path $OutputRoot $entry.Key
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $entry.Value $target -Force
}
# Only the files the built page references, not stale builds left in dist.
$settingsSource = Join-Path $releaseRoot "settings-ui"
$settingsIndex = Get-Content -LiteralPath (Join-Path $settingsSource "index.html") -Raw
$settingsFiles = @("index.html") + @([regex]::Matches($settingsIndex, "assets/[^`"']+") | ForEach-Object { $_.Value }) +
    @("assets/auto.ico", "assets/direct.ico", "assets/tekito.ico", "assets/tekito-wordmark-dark.svg",
      "assets/fonts/MPLUS1-wght.ttf")
foreach ($file in $settingsFiles | Select-Object -Unique) {
    $source = Resolve-ChildPath $settingsSource $file
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Settings page file is missing: $file" }
    $target = Join-Path $OutputRoot ("settings-ui\" + $file.Replace('/', '\'))
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $source $target -Force
}
New-Item -ItemType Directory -Path (Join-Path $OutputRoot "data") -Force | Out-Null
foreach ($pack in $packs) { Copy-Item -LiteralPath $pack.FullName (Join-Path $OutputRoot "data") -Recurse -Force }

$hashes = [ordered]@{}
foreach ($file in Get-ChildItem -LiteralPath $OutputRoot -File -Recurse |
        Where-Object { $_.FullName -notlike (Join-Path $OutputRoot "data\*") }) {
    $hashes[(Get-RelativeUnixPath $OutputRoot $file.FullName)] = Get-Sha256 $file.FullName
}
$packManifests = [ordered]@{}
foreach ($pack in $packs) {
    $manifest = Get-Content (Join-Path $pack.FullName "manifest.json") -Raw | ConvertFrom-Json
    $packManifests[[string]$manifest.pack_id] = [ordered]@{
        version = [string]$manifest.version
        license = [string]$manifest.license
    }
}
$installerName = "TEKITO-$version-full-installer.exe"
[ordered]@{
    product = "TEKITO"
    version = $version
    created_at = [DateTime]::UtcNow.ToString("o")
    single_exe_installer = $installerName
    runtime_network = "not-used"
    requires_webview2_runtime = $true
    data_packs = $packs.Count
    data_pack_manifests = $packManifests
    files = $hashes
} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutputRoot "package-manifest.json") -Encoding UTF8
& (Join-Path $OutputRoot "verify-package.ps1") -PackageRoot $OutputRoot

$ArchivePath = [IO.Path]::GetFullPath($ArchivePath)
if (Test-Path -LiteralPath $ArchivePath) { Remove-Item -LiteralPath $ArchivePath -Force }
Compress-Archive -Path (Join-Path $OutputRoot "*") -DestinationPath $ArchivePath -CompressionLevel Optimal
"$(Get-Sha256 $ArchivePath)  $([IO.Path]::GetFileName($ArchivePath))" |
    Set-Content -LiteralPath "$ArchivePath.sha256" -Encoding ASCII
Write-Host "Archive: $ArchivePath"

# The setup program finds its payload through a trailer at the end of the
# file: magic, payload length, reserved (see TekitoSetup.cpp).
$installerPath = Join-Path (Split-Path $ArchivePath -Parent) $installerName
$output = [IO.File]::Create($installerPath)
try {
    foreach ($part in @($setupStub, $ArchivePath)) {
        $stream = [IO.File]::OpenRead($part)
        try { $stream.CopyTo($output) } finally { $stream.Dispose() }
    }
    $magic = [Text.Encoding]::ASCII.GetBytes("TEKITO_PAYLOAD_V1")
    $output.Write($magic, 0, $magic.Length)
    $output.Write([BitConverter]::GetBytes([UInt64](Get-Item -LiteralPath $ArchivePath).Length), 0, 8)
    $output.Write([BitConverter]::GetBytes([UInt64]0), 0, 8)
} finally {
    $output.Dispose()
}
"$(Get-Sha256 $installerPath)  $installerName" | Set-Content -LiteralPath "$installerPath.sha256" -Encoding ASCII
& (Join-Path $OutputRoot "verify-package.ps1") -PackageRoot $OutputRoot -RequireSingleExeInstaller
Write-Host "Installer: $installerPath"
