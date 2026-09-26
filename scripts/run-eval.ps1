param(
    [string]$BuildDir,
    [string]$CMakePath,
    [string]$Corpus,
    [ValidateSet("test", "validation", "train", "all")]
    [string]$Split = "test",
    [int]$LatencySample = 200,
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildDir = if ($BuildDir) { [IO.Path]::GetFullPath($BuildDir) } else {
    Join-Path $repoRoot "out\build\eval"
}

function Resolve-BuildTool($ExplicitPath, $CommandName, $FileName) {
    if ($ExplicitPath) { return $ExplicitPath }

    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $relativePath = Join-Path "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin" $FileName
    foreach ($year in @("2026", "2022")) {
        foreach ($edition in @("BuildTools", "Community", "Professional", "Enterprise")) {
            $candidate = Join-Path $env:ProgramFiles "Microsoft Visual Studio\$year\$edition\$relativePath"
            if (Test-Path $candidate) { return $candidate }
        }
    }

    return $null
}

$cmake = Resolve-BuildTool $CMakePath "cmake" "cmake.exe"
if (-not $cmake) { throw "cmake.exe was not found. Pass -CMakePath explicitly." }

Write-Host "Preparing eval corpus..."
python (Join-Path $PSScriptRoot "prepare-eval-corpus.py")
if ($LASTEXITCODE -ne 0) { throw "prepare-eval-corpus.py failed with exit code $LASTEXITCODE" }

$corpusPath = if ($Corpus) { [IO.Path]::GetFullPath($Corpus) } else {
    Join-Path $repoRoot "eval\generated\eval_corpus.tsv"
}

Write-Host "Configuring ($buildDir)..."
& $cmake -S $repoRoot -B $buildDir -DTEKITO_BUILD_EVAL=ON -DTEKITO_BUILD_TESTS=OFF
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with exit code $LASTEXITCODE" }

Write-Host "Building tekito_core_eval ($Configuration)..."
& $cmake --build $buildDir --target tekito_core_eval --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "cmake build failed with exit code $LASTEXITCODE" }

$exe = Join-Path $buildDir "$Configuration\tekito_core_eval.exe"
if (-not (Test-Path $exe)) { $exe = Join-Path $buildDir "tekito_core_eval.exe" }
if (-not (Test-Path $exe)) { throw "Built executable was not found under $buildDir" }

$env:TEKITO_DATA_PACK_DIR = Join-Path $repoRoot "data"

Write-Host "Running eval (split=$Split)..."
& $exe --corpus $corpusPath --split $Split --latency-sample $LatencySample
exit $LASTEXITCODE
