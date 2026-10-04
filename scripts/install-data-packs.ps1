param(
    [string]$SourceRoot,
    [string]$DestinationRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $SourceRoot) {
    $SourceRoot = Join-Path $repoRoot "data"
}
if (-not $DestinationRoot) {
    if (-not $env:LOCALAPPDATA) {
        throw "LOCALAPPDATA is not available. Specify -DestinationRoot."
    }
    $DestinationRoot = Join-Path $env:LOCALAPPDATA "TEKITO\data"
}

$SourceRoot = (Resolve-Path $SourceRoot).Path
# Side by side, as a release carries them, or by language (en, ja, common),
# as the repository keeps them; they are installed side by side.
$sourceFolders = @($SourceRoot) + @("en", "ja", "common" | ForEach-Object { Join-Path $SourceRoot $_ } |
    Where-Object { Test-Path -LiteralPath $_ })
$packDirectories = @($sourceFolders | ForEach-Object { Get-ChildItem -LiteralPath $_ -Directory } |
    Where-Object { Test-Path (Join-Path $_.FullName "manifest.json") })
if ($packDirectories.Count -eq 0) {
    throw "No Data Pack manifests were found under $SourceRoot."
}

# Puts one file in place. Apps with TEKITO loaded keep Data Pack files open:
# the English ones can be overwritten, the Japanese ones (mapped into
# memory) only renamed. A file that cannot be overwritten is moved aside as
# "<name>.old-<id>", which the next install removes.
function Install-PackFile([string]$Source, [string]$Target) {
    try {
        Copy-Item -LiteralPath $Source $Target -Force -ErrorAction Stop
        return
    } catch {
        if (-not (Test-Path -LiteralPath $Target)) { throw }
    }
    try {
        Rename-Item -LiteralPath $Target ("{0}.old-{1}" -f (Split-Path $Target -Leaf), [guid]::NewGuid().ToString("N")) -ErrorAction Stop
    } catch {
        throw "$Target is in use. Close the apps you type in (or sign out and back in), then install again."
    }
    Copy-Item -LiteralPath $Source $Target -Force -ErrorAction Stop
}

function Require-ManifestValue($Manifest, [string]$Name) {
    $value = $Manifest.$Name
    if ($null -eq $value -or [string]::IsNullOrWhiteSpace([string]$value)) {
        throw "Required manifest field is missing: $Name"
    }
    return [string]$value
}

$stagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("tekito-data-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $stagingRoot | Out-Null
try {
    foreach ($pack in $packDirectories) {
        $manifestPath = Join-Path $pack.FullName "manifest.json"
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $packId = Require-ManifestValue $manifest "pack_id"
        [void](Require-ManifestValue $manifest "version")
        [void](Require-ManifestValue $manifest "language")
        [void](Require-ManifestValue $manifest "type")
        [void](Require-ManifestValue $manifest "license")
        $noticeFile = Require-ManifestValue $manifest "notice_file"
        $dataFile = Require-ManifestValue $manifest "file"
        $dataPath = Join-Path $pack.FullName $dataFile
        $noticePath = Join-Path $pack.FullName $noticeFile
        if (-not (Test-Path $dataPath) -or -not (Test-Path $noticePath)) {
            throw "Pack $packId is missing data or NOTICE."
        }
        $expectedData = ([string]$manifest.sha256.file).ToUpperInvariant()
        $actualData = (Get-FileHash -LiteralPath $dataPath -Algorithm SHA256).Hash.ToUpperInvariant()
        if ($actualData -ne $expectedData) { throw "Checksum mismatch in pack $packId." }
        # Packs read front to back (sorted-tsv, ja-lm) have no index file.
        if ($manifest.PSObject.Properties.Name -contains "index_file") {
            $indexPath = Join-Path $pack.FullName ([string]$manifest.index_file)
            if (-not (Test-Path $indexPath)) { throw "Pack $packId is missing its index." }
            $actualIndex = (Get-FileHash -LiteralPath $indexPath -Algorithm SHA256).Hash.ToUpperInvariant()
            if ($actualIndex -ne ([string]$manifest.sha256.index).ToUpperInvariant()) {
                throw "Checksum mismatch in pack $packId."
            }
        }

        $stagingPack = Join-Path $stagingRoot $pack.Name
        Copy-Item -LiteralPath $pack.FullName -Destination $stagingPack -Recurse -Force
        Write-Host "Validated $packId $($manifest.version)"
    }

    New-Item -ItemType Directory -Path $DestinationRoot -Force | Out-Null
    foreach ($pack in $packDirectories) {
        $stagingPack = Join-Path $stagingRoot $pack.Name
        $destinationPack = Join-Path $DestinationRoot $pack.Name
        New-Item -ItemType Directory -Path $destinationPack -Force | Out-Null
        # Copies moved aside by an earlier install, once nothing uses them.
        Get-ChildItem -LiteralPath $destinationPack -Filter "*.old-*" -File -Recurse -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
        foreach ($file in @(Get-ChildItem -LiteralPath $stagingPack -File -Recurse)) {
            $target = Join-Path $destinationPack $file.FullName.Substring($stagingPack.Length + 1)
            New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
            Install-PackFile $file.FullName $target
        }
    }
    Write-Host "TEKITO offline Data Packs installed at $DestinationRoot"
    Write-Host "User Dictionary, User Learning, and Settings remain in UserData and are not modified."
}
finally {
    if (Test-Path $stagingRoot) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
}
