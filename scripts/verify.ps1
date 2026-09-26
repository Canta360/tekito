# Builds TEKITO, runs the tests and checks the rules every change must keep.
# Run it before committing. It never registers the input method or touches
# the Windows input settings.
param(
    [string]$BuildDir,
    [string]$CMakePath,
    [string]$CTestPath
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildDir = if ($BuildDir) { [IO.Path]::GetFullPath($BuildDir) } else {
    Join-Path $repoRoot "out\build\windows-x64-debug"
}

# CMake and CTest from PATH, or the copies Visual Studio installs.
function Resolve-BuildTool($ExplicitPath, $CommandName) {
    if ($ExplicitPath) { return $ExplicitPath }
    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($year in @("2026", "2022")) {
        foreach ($edition in @("BuildTools", "Community", "Professional", "Enterprise")) {
            $candidate = Join-Path $env:ProgramFiles `
                "Microsoft Visual Studio\$year\$edition\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\$CommandName.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    throw "$CommandName was not found. Install CMake or the Visual Studio C++ workload."
}

# Fails when any file under $Paths contains $Pattern.
function Assert-Absent($Rule, $Pattern, [string[]]$Paths) {
    $hits = foreach ($path in $Paths) {
        Get-ChildItem (Join-Path $repoRoot $path) -Recurse -File -Include *.cpp, *.h |
            Select-String -Pattern $Pattern
    }
    if ($hits) {
        $hits | ForEach-Object { Write-Host "  $($_.Path):$($_.LineNumber): $($_.Line.Trim())" }
        throw "Rule broken: $Rule"
    }
    Write-Host "ok  $Rule"
}

$cmake = Resolve-BuildTool $CMakePath "cmake"
$ctest = Resolve-BuildTool $CTestPath "ctest"

$scriptDirs = @("installer", "scripts") | ForEach-Object { Join-Path $repoRoot $_ }
foreach ($script in Get-ChildItem $scriptDirs -Filter *.ps1 -File) {
    $tokens = $null
    $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile($script.FullName, [ref]$tokens, [ref]$errors) | Out-Null
    if ($errors.Count -gt 0) { throw "PowerShell parse error in $($script.FullName): $($errors[0].Message)" }
}
Write-Host "ok  PowerShell scripts parse"

Push-Location $repoRoot
try {
    if (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
        & $cmake --preset windows-x64-debug
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    }
    & $cmake --build $buildDir --config Debug
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
    Write-Host "ok  Build"

    $env:TEKITO_DATA_PACK_DIR = Join-Path $repoRoot "data"
    & $ctest --test-dir $buildDir -C Debug --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)" }
    Write-Host "ok  Tests"
} finally {
    Pop-Location
}

$product = @("src", "apps")

# TEKITO is a TSF text service. It must not fall back to watching or
# faking keystrokes the way the retired overlay prototype did.
Assert-Absent "no keyboard hooks or synthesized input" `
    "SetWindowsHookEx|WH_KEYBOARD|SendInput|keybd_event|mouse_event" $product

# Typing is processed offline; nothing in the input path talks to a network.
Assert-Absent "no network access in the input method or its data" `
    "WinHttp|WinINet|InternetOpen|URLDownloadToFile|curl_easy|HttpClient" `
    @("src\Core", "src\Tsf", "src\UserData", "src\Dictionary")

# Diagnostics may name events and timings, never what the user typed.
Assert-Absent "no typed text in trace output" `
    "Trace(Hr)?\([^\n]*(rawText_|input\.character|candidates_\[|action\.text|\.text\.c_str)" `
    @("src\Tsf")

# Vocabulary lives in Data Packs, not in if-statements.
Assert-Absent "no word-specific special cases in the engine" `
    '(lower|rawText|word)\s*==\s*L?"' @("src\Core")

Write-Host "TEKITO verification passed."
