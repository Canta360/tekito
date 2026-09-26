[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$roots = @(
    (Join-Path ${env:ProgramFiles} "Microsoft\EdgeWebView\Application"),
    (Join-Path ${env:ProgramFiles(x86)} "Microsoft\EdgeWebView\Application"),
    (Join-Path $env:LOCALAPPDATA "Microsoft\EdgeWebView\Application")
) | Where-Object { $_ }

$runtime = $roots |
    Where-Object { Test-Path -LiteralPath $_ } |
    ForEach-Object { Get-ChildItem -LiteralPath $_ -Recurse -Filter "msedgewebview2.exe" -File -ErrorAction SilentlyContinue } |
    Select-Object -First 1

$clientId = "{F3017226-FE2A-4A2A-8C2B-7D8D7B5B4A3F}"
$registryRuntime = @(
    "HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$clientId",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\$clientId",
    "HKCU:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$clientId",
    "HKCU:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\$clientId"
) |
    ForEach-Object { Get-ItemProperty -LiteralPath $_ -Name pv -ErrorAction SilentlyContinue } |
    Where-Object { $_.pv -and $_.pv -ne "0.0.0.0" } |
    Select-Object -First 1

if (-not $runtime -and -not $registryRuntime) {
    throw "Microsoft Edge WebView2 Runtime was not found. Install the WebView2 Runtime, then run install.ps1 again."
}

if ($runtime) {
    Write-Host "WebView2 Runtime found: $($runtime.FullName)"
} else {
    Write-Host "WebView2 Runtime found in Edge Update registry: $($registryRuntime.pv)"
}
