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
$packDirectories = @(Get-ChildItem -LiteralPath $SourceRoot -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName "manifest.json") })
if ($packDirectories.Count -eq 0) {
    throw "No Data Pack manifests were found under $SourceRoot."
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
        $indexFile = Require-ManifestValue $manifest "index_file"
        $dataPath = Join-Path $pack.FullName $dataFile
        $indexPath = Join-Path $pack.FullName $indexFile
        $noticePath = Join-Path $pack.FullName $noticeFile
        if (-not (Test-Path $dataPath) -or -not (Test-Path $indexPath) -or -not (Test-Path $noticePath)) {
            throw "Pack $packId is missing data, index, or NOTICE."
        }
        $expectedData = ([string]$manifest.sha256.file).ToUpperInvariant()
        $expectedIndex = ([string]$manifest.sha256.index).ToUpperInvariant()
        $actualData = (Get-FileHash -LiteralPath $dataPath -Algorithm SHA256).Hash.ToUpperInvariant()
        $actualIndex = (Get-FileHash -LiteralPath $indexPath -Algorithm SHA256).Hash.ToUpperInvariant()
        if ($actualData -ne $expectedData -or $actualIndex -ne $expectedIndex) {
            throw "Checksum mismatch in pack $packId."
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
        Copy-Item -Path (Join-Path $stagingPack "*") -Destination $destinationPack -Recurse -Force
    }
    Write-Host "TEKITO offline Data Packs installed at $DestinationRoot"
    Write-Host "User Dictionary, User Learning, and Settings remain in UserData and are not modified."
}
finally {
    if (Test-Path $stagingRoot) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
}
