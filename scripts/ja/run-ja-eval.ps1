# Measures Japanese conversion: builds the eval corpus
# (prepare-japanese-eval.py), builds tekito_ja_eval and runs it on the
# japanese-core pack (prepare-japanese-packs.ps1 builds the pack).
param(
    [string]$BuildDir,
    [string]$CMakePath,
    [string]$Pack,
    [int]$ShowMisses = 0,
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$buildDir = if ($BuildDir) { [IO.Path]::GetFullPath($BuildDir) } else { Join-Path $repoRoot "out\build\eval" }
$pack = if ($Pack) { [IO.Path]::GetFullPath($Pack) } else { Join-Path $repoRoot "data\ja\japanese-core" }
if (-not (Test-Path (Join-Path $pack "dictionary.bin"))) {
    throw "No japanese-core pack at $pack. Run scripts\ja\prepare-japanese-packs.ps1 first."
}

function Resolve-CMake($ExplicitPath) {
    if ($ExplicitPath) { return $ExplicitPath }
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($year in @("2026", "2022")) {
        foreach ($edition in @("BuildTools", "Community", "Professional", "Enterprise")) {
            $candidate = Join-Path $env:ProgramFiles `
                "Microsoft Visual Studio\$year\$edition\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    throw "cmake.exe was not found. Pass -CMakePath explicitly."
}
$cmake = Resolve-CMake $CMakePath

python (Join-Path $PSScriptRoot "prepare-japanese-eval.py")
if ($LASTEXITCODE -ne 0) { throw "prepare-japanese-eval.py failed with exit code $LASTEXITCODE" }

& $cmake -S $repoRoot -B $buildDir -DTEKITO_BUILD_EVAL=ON -DTEKITO_BUILD_TESTS=OFF | Out-Null
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with exit code $LASTEXITCODE" }
& $cmake --build $buildDir --target tekito_ja_eval --config $Configuration | Out-Null
if ($LASTEXITCODE -ne 0) { throw "cmake build failed with exit code $LASTEXITCODE" }

$exe = Join-Path $buildDir "$Configuration\tekito_ja_eval.exe"
$corpus = Join-Path $repoRoot "eval\generated\japanese_eval.tsv"
& $exe --pack $pack --corpus $corpus --show-misses $ShowMisses
exit $LASTEXITCODE
