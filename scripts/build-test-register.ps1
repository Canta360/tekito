param(
    [switch]$Register
)

$ErrorActionPreference = "Stop"

& (Join-Path $PSScriptRoot "verify.ps1")

if ($Register) {
    & (Join-Path $PSScriptRoot "register-debug.ps1")
}
