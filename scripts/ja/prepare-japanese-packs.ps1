# Downloads the Mozc OSS dictionary at a pinned commit into
# .cache\tekito-data and builds the japanese-core pack from it
# (scripts\ja\build-japanese-packs.py). The pack is too large for the
# repository; TEKITO types Japanese without it, but cannot convert to kanji.
# Then builds the language model conversion uses
# (scripts\ja\build-japanese-lm.py, from the Leipzig corpora) and the meaning
# packs the candidate list shows beside a word
# (scripts\ja\build-japanese-meaning-packs.py), unless -SkipMeanings.
param(
    [string]$Commit = "b9c3fcbd6d76b19649ef572324fa9da2559bc18e",
    [string]$Output,
    [switch]$ForceDownload,
    [switch]$SkipMeanings
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if (-not $Output) { $Output = Join-Path $repoRoot "data\ja\japanese-core" }
$mozc = Join-Path $repoRoot ".cache\tekito-data\mozc-$Commit"

$files = @("LICENSE", "src/data/dictionary_oss/README.txt", "src/data/dictionary_oss/id.def",
           "src/data/dictionary_oss/connection_single_column.txt",
           "src/data/dictionary_oss/evaluation.tsv", "src/data/rules/segmenter.def") +
         (0..9 | ForEach-Object { "src/data/dictionary_oss/dictionary0$_.txt" })
foreach ($file in $files) {
    # LICENSE sits at the top of the repository, the rest under src/data.
    $relative = if ($file -eq "LICENSE") { "LICENSE" } else { $file.Substring("src/data/".Length) }
    $target = Join-Path $mozc $relative
    if ((Test-Path $target) -and -not $ForceDownload) { continue }
    New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
    $url = "https://raw.githubusercontent.com/google/mozc/$Commit/$file"
    Write-Host "Downloading $file"
    Invoke-WebRequest -Uri $url -OutFile $target -UseBasicParsing
}

python (Join-Path $PSScriptRoot "build-japanese-packs.py") --mozc $mozc --commit $Commit --out $Output
if ($LASTEXITCODE -ne 0) { throw "build-japanese-packs.py failed with exit code $LASTEXITCODE" }
Write-Host "Built $Output"

python (Join-Path $PSScriptRoot "build-japanese-lm.py")
if ($LASTEXITCODE -ne 0) { throw "build-japanese-lm.py failed with exit code $LASTEXITCODE" }

if (-not $SkipMeanings) {
    python (Join-Path $PSScriptRoot "build-japanese-meaning-packs.py")
    if ($LASTEXITCODE -ne 0) { throw "build-japanese-meaning-packs.py failed with exit code $LASTEXITCODE" }
}
