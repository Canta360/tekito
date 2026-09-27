# Registers a locally built Tekito.Tsf.dll as an input method for the current
# user, for trying out a build. Run from an administrator PowerShell; undo it
# with unregister-debug.ps1.
param(
    [string]$DllPath,
    [switch]$SkipLaunch
)

$ErrorActionPreference = "Stop"
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Administrator PowerShell is required."
}

if (-not $DllPath) {
    # The newest Trace build if there is one, otherwise Debug, then Release.
    $repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
    $outRoot = Join-Path $repoRoot "out\build"
    $trace = Get-ChildItem -LiteralPath $outRoot -Filter "Tekito.Tsf.Trace*.dll" -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
    $DllPath = @(
        $trace,
        (Join-Path $outRoot "windows-x64-debug\Debug\Tekito.Tsf.dll"),
        (Join-Path $outRoot "windows-x64-release\Release\Tekito.Tsf.dll")
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if (-not $DllPath) { throw "No Tekito.Tsf.dll was found under $outRoot. Build first, or pass -DllPath." }
}

$resolvedDll = (Resolve-Path $DllPath).Path
$appPath = Join-Path (Split-Path $resolvedDll -Parent) "TEKITO.exe"
if (-not (Test-Path -LiteralPath $appPath)) {
    throw "TEKITO.exe was not found next to $resolvedDll."
}

& (Join-Path $PSScriptRoot "unregister-debug.ps1") -DllPath $resolvedDll

Write-Host "Registering $resolvedDll"
$regsvr32 = Join-Path $env:WINDIR "System32\regsvr32.exe"
$process = Start-Process -FilePath $regsvr32 -ArgumentList @('/s', $resolvedDll) -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -ne 0) {
    throw "regsvr32 failed with exit code $($process.ExitCode)"
}

if ((Split-Path $resolvedDll -Leaf) -match '\.Trace[0-9]*\.dll$') {
    $tsfLogPath = Join-Path $env:TEMP "TekitoTsf.log"
    Remove-Item -LiteralPath $tsfLogPath -Force -ErrorAction SilentlyContinue
    Write-Host "Trace log reset: $tsfLogPath"
}

$tip = "0409:{6F67E5C8-A873-4B69-8EC3-26DF00F642F1}{8D99240A-5C9C-4ED8-8DE4-85F21DF23763}"
$languages = Get-WinUserLanguageList
$english = $languages | Where-Object { $_.LanguageTag -eq "en-US" } | Select-Object -First 1
if (-not $english) {
    $english = (New-WinUserLanguageList en-US)[0]
    $languages.Add($english)
}
$changed = $false
if ($english.InputMethodTips -notcontains $tip) {
    $english.InputMethodTips.Add($tip)
    $changed = $true
    Write-Host "Added TEKITO English to the English (United States) keyboards."
}
# The Japanese profile goes to the Japanese keyboards, if Japanese is one of
# the user's languages; TEKITO does not add the language itself.
$japaneseTip = "0411:{6F67E5C8-A873-4B69-8EC3-26DF00F642F1}{2876747C-FA52-4CCC-B308-F406190F8CDE}"
$japanese = $languages | Where-Object { $_.LanguageTag -eq "ja" -or $_.LanguageTag -eq "ja-JP" } | Select-Object -First 1
if ($japanese -and $japanese.InputMethodTips -notcontains $japaneseTip) {
    $japanese.InputMethodTips.Add($japaneseTip)
    $changed = $true
    Write-Host "Added TEKITO to the Japanese keyboards."
} elseif (-not $japanese) {
    Write-Host "Japanese is not among your languages; add it in Settings to try TEKITO's Japanese profile."
}
if ($changed) { Set-WinUserLanguageList $languages -Force }

Write-Host "Registered. Sign out and back in, or restart the apps you want to try it in."
Write-Host "Settings: & '$appPath' --settings"

if (-not $SkipLaunch) {
    Start-Process -FilePath $appPath -ArgumentList "--settings"
}
