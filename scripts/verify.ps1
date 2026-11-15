# Builds TEKITO, runs the tests and checks the rules every change must keep.
# Run it before committing. It never registers the input method or touches
# the Windows input settings.
param(
    [string]$BuildDir,
    [string]$CMakePath,
    [string]$CTestPath,
    # windows-x64-vs2022-debug where only Visual Studio 2022 is installed
    # (GitHub's Windows runners).
    [string]$Preset = "windows-x64-debug",
    # Release builds and tests what is shipped (the preset has both).
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildDir = if ($BuildDir) { [IO.Path]::GetFullPath($BuildDir) } else {
    Join-Path $repoRoot "out\build\$Preset"
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

# Fails when any file under $Paths contains $Pattern, but on a line right
# after a comment with "verify-allow" in it (an exception the rule names).
function Assert-Absent($Rule, $Pattern, [string[]]$Paths) {
    $hits = foreach ($path in $Paths) {
        Get-ChildItem (Join-Path $repoRoot $path) -Recurse -File -Include *.cpp, *.h |
            Select-String -Pattern $Pattern -Context 1, 0 |
            Where-Object { -not ($_.Context.PreContext -match "verify-allow") }
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
        & $cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    }
    & $cmake --build $buildDir --config $Configuration
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
    Write-Host "ok  Build"

    $env:TEKITO_DATA_PACK_DIR = Join-Path $repoRoot "data"
    & $ctest --test-dir $buildDir -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)" }
    Write-Host "ok  Tests"
} finally {
    Pop-Location
}

$product = @("src", "apps")

# TEKITO is a TSF text service. It must not fall back to watching or
# faking keystrokes the way the retired overlay prototype did. Two
# exceptions, each marked verify-allow: Left keys that put the caret back
# between brackets in older edit controls TSF only bridges to
# (TextService.cpp), and a keyboard hook on the typing application's own
# thread, only while the left and right Alt switch is on, as TSF passes no
# Alt to text services; it keeps no key (TextServiceJapanese.cpp).
Assert-Absent "no keyboard hooks or synthesized input" `
    "SetWindowsHookEx|WH_KEYBOARD|SendInput|keybd_event|mouse_event" $product

# Typing is processed offline; nothing in the input path talks to a network.
Assert-Absent "no network access in the input method or its data" `
    "WinHttp|WinINet|InternetOpen|URLDownloadToFile|curl_easy|HttpClient" `
    @("src\Core", "src\Tsf", "src\UserData", "src\Dictionary")

# Diagnostics may name events and timings, never what the user typed.
Assert-Absent "no typed text in trace output" `
    "Trace(Hr)?\([^\n]*(rawText_|input\.character|candidates_\[|action\.text|\.text\.c_str|\+\s*text\b|\+\s*reading\b|Preedit\(\))" `
    @("src\Tsf")

# Vocabulary lives in Data Packs, not in if-statements.
Assert-Absent "no word-specific special cases in the engine" `
    '(lower|rawText|word)\s*==\s*L?"' @("src\Core")

# Fails when any file under $Paths (of the kinds in $Include) contains
# $Pattern; for text that is not code.
function Assert-AbsentIn($Rule, $Pattern, [string[]]$Paths, [string[]]$Include) {
    $hits = foreach ($path in $Paths) {
        $full = Join-Path $repoRoot $path
        if (-not (Test-Path $full)) { continue }
        Get-ChildItem $full -Recurse -File -Include $Include |
            Where-Object { $_.FullName -notmatch '\\(node_modules|dist)\\' } |
            Select-String -Pattern $Pattern
    }
    if ($hits) {
        $hits | ForEach-Object { Write-Host "  $($_.Path):$($_.LineNumber): $($_.Line.Trim())" }
        throw "Rule broken: $Rule"
    }
    Write-Host "ok  $Rule"
}

# What users read tells them what happens, never to type or write
# something (入力してください, 書いてください): Settings, setup, the
# input method's own messages and the documents for users. The guides for
# contributors are not for users.
Assert-AbsentIn "no telling users to type or write" `
    "\u5165\u529B\u3057\u3066\u304F\u3060\u3055\u3044|\u66F8\u3044\u3066\u304F\u3060\u3055\u3044" `
    @("apps\tekito-settings\ui\src", "apps\tekito-settings\main.cpp", "installer", "src", "README.md", "README_JP.md", "PRIVACY.md", "PRIVACY_JP.md") `
    @("*.cpp", "*.h", "*.rc", "*.js", "*.jsx", "*.md", "*.ps1", "*.json")

# One version everywhere it is written: CMake, the Settings page and the
# file names the README and the install guide give.
$cmakeVersion = [regex]::Match((Get-Content (Join-Path $repoRoot "CMakeLists.txt") -Raw), 'project\(TEKITO VERSION ([0-9.]+)').Groups[1].Value
$versions = [ordered]@{
    "apps\tekito-settings\ui\package.json" = (Get-Content (Join-Path $repoRoot "apps\tekito-settings\ui\package.json") -Raw | ConvertFrom-Json).version
    "apps\tekito-settings\ui\package-lock.json" = (Get-Content (Join-Path $repoRoot "apps\tekito-settings\ui\package-lock.json") -Raw | ConvertFrom-Json -AsHashtable)["version"]
    "apps\tekito-settings\ui\src\devHost.js" = [regex]::Match((Get-Content (Join-Path $repoRoot "apps\tekito-settings\ui\src\devHost.js") -Raw), '\n\s*version: "([0-9.]+)"').Groups[1].Value
}
foreach ($doc in @("README.md", "README_JP.md", "installer\INSTALL_JP.md")) {
    foreach ($match in [regex]::Matches((Get-Content (Join-Path $repoRoot $doc) -Raw), 'TEKITO-([0-9]+\.[0-9]+\.[0-9]+)-')) {
        if ($match.Groups[1].Value -ne $cmakeVersion) { $versions[$doc] = $match.Groups[1].Value }
    }
}
$wrong = $versions.GetEnumerator() | Where-Object { $_.Value -ne $cmakeVersion }
if (-not $cmakeVersion -or $wrong) {
    $wrong | ForEach-Object { Write-Host "  $($_.Key): $($_.Value) (CMakeLists.txt: $cmakeVersion)" }
    throw "Rule broken: one version everywhere"
}
Write-Host "ok  one version everywhere ($cmakeVersion)"

Write-Host "TEKITO verification passed."
