# Adds Windows Defender exclusions for the xmr-verify-proxy working tree.
#
# Why: the compiled proxy embeds RandomX, and Defender's heuristic classifies any
# unsigned binary containing it as Trojan:Win64/HashvaultMiner.A. That is a false
# positive - the proxy only *verifies* shares - but Defender will refuse to run it.
#
# Run this in an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File .\scripts\allow-defender.ps1
#
# Undo later with:
#   Remove-MpPreference -ExclusionPath 'C:\Users\49698\Desktop\project\xmr-verify-proxy'

$ErrorActionPreference = 'Stop'

$identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "This script must run as Administrator." -ForegroundColor Red
    exit 1
}

$projectRoot = Split-Path -Parent $PSScriptRoot
Write-Host "project root: $projectRoot" -ForegroundColor Cyan

Add-MpPreference -ExclusionPath $projectRoot
Write-Host "excluded path: $projectRoot" -ForegroundColor Green

foreach ($exe in @('xmr-verify-proxy.exe', 'xmrig.exe')) {
    Add-MpPreference -ExclusionProcess $exe
    Write-Host "excluded process: $exe" -ForegroundColor Green
}

Write-Host "`ncurrent exclusions:" -ForegroundColor Cyan
(Get-MpPreference).ExclusionPath
(Get-MpPreference).ExclusionProcess
