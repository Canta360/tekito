param(
    [switch]$ForceDownload,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$cache = Join-Path $repoRoot ".cache\tekito-data"
New-Item -ItemType Directory -Force $cache | Out-Null

function Get-Source([string]$Name, [string]$Url) {
    $destination = Join-Path $cache $Name
    if ($ForceDownload -or -not (Test-Path -LiteralPath $destination)) {
        Write-Host "Downloading $Name"
        & curl.exe -L --fail --retry 3 -o $destination $Url
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $Url" }
    }
}

Get-Source "eng_news_2025_1M.tar.gz" "https://downloads.wortschatz-leipzig.de/corpora/eng_news_2025_1M.tar.gz"
Get-Source "WordNet-3.0.tar.gz" "https://wordnetcode.princeton.edu/3.0/WordNet-3.0.tar.gz"
Get-Source "cmudict.dict" "https://raw.githubusercontent.com/cmusphinx/cmudict/master/cmudict.dict"
Get-Source "cmudict-LICENSE" "https://raw.githubusercontent.com/cmusphinx/cmudict/master/LICENSE"
Get-Source "cldr-common-48.2.zip" "https://unicode.org/Public/cldr/48.2/cldr-common-48.2.zip"
Get-Source "unicode-LICENSE" "https://raw.githubusercontent.com/unicode-org/cldr/release-48-2/LICENSE"
Get-Source "emoji-test-17.0.0.txt" "https://www.unicode.org/Public/17.0.0/emoji/emoji-test.txt"
Get-Source "geonames-allCountries.zip" "https://download.geonames.org/export/dump/allCountries.zip"
Get-Source "geonames-readme.txt" "https://download.geonames.org/export/dump/readme.txt"
Get-Source "raw-wiktextract-data.jsonl.gz" "https://kaikki.org/dictionary/raw-wiktextract-data.jsonl.gz"

$wikipedia = Join-Path $cache "wikipedia-common-misspellings.json"
if ($ForceDownload -or -not (Test-Path -LiteralPath $wikipedia)) {
    $url = "https://en.wikipedia.org/w/api.php?action=parse&page=Wikipedia%3ALists_of_common_misspellings%2FFor_machines&prop=wikitext%7Crevid&format=json&formatversion=2"
    & curl.exe -L --fail --retry 3 -o $wikipedia $url
    if ($LASTEXITCODE -ne 0) { throw "Wikipedia download failed." }
}

$leipzig = Join-Path $cache "leipzig"
$wordnet = Join-Path $cache "wordnet"
$cldr = Join-Path $cache "cldr"
if ($ForceDownload -or -not (Test-Path (Join-Path $leipzig "eng_news_2025_1M"))) {
    New-Item -ItemType Directory -Force $leipzig | Out-Null
    & tar.exe -xzf (Join-Path $cache "eng_news_2025_1M.tar.gz") -C $leipzig
}
if ($ForceDownload -or -not (Test-Path (Join-Path $wordnet "WordNet-3.0"))) {
    New-Item -ItemType Directory -Force $wordnet | Out-Null
    & tar.exe -xzf (Join-Path $cache "WordNet-3.0.tar.gz") -C $wordnet
}
if ($ForceDownload -or -not (Test-Path (Join-Path $cldr "common"))) {
    New-Item -ItemType Directory -Force $cldr | Out-Null
    & tar.exe -xf (Join-Path $cache "cldr-common-48.2.zip") -C $cldr
}

if (-not $SkipBuild) {
    & python (Join-Path $PSScriptRoot "build-full-data-packs.py")
    if ($LASTEXITCODE -ne 0) { throw "Data Pack build failed." }
    & (Join-Path $PSScriptRoot "install-data-packs.ps1") -DestinationRoot (Join-Path $cache "validation-install")
}
