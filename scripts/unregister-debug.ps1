param(
    [string]$DllPath
)

$ErrorActionPreference = "Stop"
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Administrator PowerShell is required."
}

Get-Process -Name "TEKITO","TekitoTray","TekitoSettings" -ErrorAction SilentlyContinue |
    Stop-Process -ErrorAction SilentlyContinue
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "TEKITO" -ErrorAction SilentlyContinue

$regsvr32 = Join-Path $env:WINDIR "System32\regsvr32.exe"
$targets = @()
if ($DllPath) {
    $targets += $DllPath
}
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$targets += Get-ChildItem -LiteralPath (Join-Path $repoRoot "build") -Filter "Tekito.Tsf*.dll" -Recurse -File -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty FullName
$targets += Get-ChildItem -LiteralPath (Join-Path $repoRoot "out") -Filter "Tekito.Tsf*.dll" -Recurse -File -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty FullName
$targets = $targets | Select-Object -Unique

foreach ($target in $targets) {
    if (-not (Test-Path $target)) {
        Write-Host "Skipping missing $target"
        continue
    }

    $resolvedDll = (Resolve-Path $target).Path
    Write-Host "Unregistering $resolvedDll"
    $process = Start-Process -FilePath $regsvr32 -ArgumentList @('/u', '/s', $resolvedDll) -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        Write-Warning "Skipping unregister for $resolvedDll (exit code $($process.ExitCode))."
    }
}

$tip = "0409:{6F67E5C8-A873-4B69-8EC3-26DF00F642F1}{8D99240A-5C9C-4ED8-8DE4-85F21DF23763}"
$languages = Get-WinUserLanguageList
$changed = $false
foreach ($language in $languages) {
    if ($language.InputMethodTips -contains $tip) {
        [void]$language.InputMethodTips.Remove($tip)
        $changed = $true
    }
}
if ($changed) {
    Set-WinUserLanguageList $languages -Force
    Write-Host "Removed TEKITO from InputMethodTips."
}

Write-Host "TEKITO TSF unregistered."
