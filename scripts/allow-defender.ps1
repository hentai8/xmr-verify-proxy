# Grants Windows Defender the exclusions this project needs, then verifies them.
#
# Why this is needed:
#   xmr-verify-proxy embeds RandomX, and Defender classifies any unsigned binary
#   containing it as Trojan:Win64/HashvaultMiner.A. xmrig.exe is flagged as
#   Trojan:Win64/DisguisedXMRigMiner. Both are false positives (this tool only
#   *verifies* shares), but Defender will delete or refuse to run them.
#
# usage (it will ask for admin by itself):
#   powershell -ExecutionPolicy Bypass -File .\scripts\allow-defender.ps1
#   powershell -ExecutionPolicy Bypass -File .\scripts\allow-defender.ps1 -WithXmrig
#
# -WithXmrig also downloads the official XMRig release into .\testbench, checks
# its SHA256 against the published SHA256SUMS, and confirms it survives.
#
# Exit code 0 = every exclusion is verifiably active.

[CmdletBinding()]
param(
    [switch]$WithXmrig,
    [switch]$NoElevate,
    [string]$XmrigVersion = '6.27.0',
    [string]$ResultFile = ''
)

$ErrorActionPreference = 'Stop'

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Write-Both {
    param([string]$Text, [string]$Color = 'Gray')
    Write-Host $Text -ForegroundColor $Color
    if ($script:log) { Add-Content -Path $script:log -Value $Text -Encoding UTF8 }
}

function Show-ManualSteps {
    $root = Split-Path -Parent $PSScriptRoot
    Write-Host ""
    Write-Host "Add these exclusions by hand instead:" -ForegroundColor Yellow
    Write-Host "  1. Open Windows Security -> Virus & threat protection"
    Write-Host "  2. Under 'Virus & threat protection settings' click 'Manage settings'"
    Write-Host "  3. Scroll to 'Exclusions' -> 'Add or remove exclusions' -> 'Add an exclusion'"
    Write-Host "  4. Choose 'Folder' and add:"
    Write-Host "       $root" -ForegroundColor White
    Write-Host "  5. Choose 'Process' and add:"
    Write-Host "       xmrig.exe" -ForegroundColor White
    Write-Host "       xmr-verify-proxy.exe" -ForegroundColor White
}

function Show-ThirdPartyAvHint {
    $av = @()
    try {
        $av = @(Get-CimInstance -Namespace root\SecurityCenter2 -ClassName AntiVirusProduct -ErrorAction Stop |
                Select-Object -ExpandProperty displayName)
    } catch { }
    $others = @($av | Where-Object { $_ -and $_ -notmatch 'Defender' })
    Write-Host ""
    Write-Host "Another antivirus is present on this machine:" -ForegroundColor Yellow
    foreach ($o in $others) { Write-Host "  - $o" -ForegroundColor White }
    Write-Host "Defender exclusions do not affect it. If xmrig.exe (or this proxy) disappears the"
    Write-Host "moment it is launched, that product is blocking it - add the folder to ITS trust"
    Write-Host "list, e.g. Huorong: 防护中心 -> 信任区 -> 添加目录 / 添加文件."
}

# ---------------------------------------------------------------------------
# Elevate if needed
# ---------------------------------------------------------------------------
if (-not (Test-Admin)) {
    if ($NoElevate) {
        Write-Host "This script must run as Administrator." -ForegroundColor Red
        exit 1
    }
    Write-Host "Requesting administrator rights (accept the UAC prompt)..." -ForegroundColor Yellow
    $root = Split-Path -Parent $PSScriptRoot
    $logDir = Join-Path $root 'logs'
    New-Item -ItemType Directory -Force $logDir | Out-Null
    $resultFile = Join-Path $logDir 'defender-exclusions.txt'
    Remove-Item $resultFile -ErrorAction SilentlyContinue

    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
                 '-NoElevate', '-ResultFile', "`"$resultFile`"")
    if ($WithXmrig) { $argList += '-WithXmrig' }
    $argList += @('-XmrigVersion', $XmrigVersion)

    try {
        $proc = Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait -PassThru
    } catch {
        Write-Host ""
        Write-Host "Elevation was cancelled or blocked." -ForegroundColor Red
        Show-ManualSteps
        exit 1
    }

    if (Test-Path $resultFile) {
        Write-Host ""
        Write-Host "---- output from the elevated run ----" -ForegroundColor Cyan
        Get-Content $resultFile | ForEach-Object { Write-Host $_ }
    }
    if ($proc.ExitCode -ne 0) {
        Write-Host ""
        Write-Host "The elevated run reported a problem (exit $($proc.ExitCode))." -ForegroundColor Red
        Show-ManualSteps
    }
    exit $proc.ExitCode
}

# ---------------------------------------------------------------------------
# Elevated path
# ---------------------------------------------------------------------------
$root = Split-Path -Parent $PSScriptRoot
$script:log = if ($ResultFile) { $ResultFile } else { Join-Path $root 'logs\defender-exclusions.txt' }
New-Item -ItemType Directory -Force (Join-Path $root 'logs') | Out-Null
Set-Content -Path $script:log -Value "xmr-verify-proxy Defender exclusions $(Get-Date -Format o)" -Encoding UTF8


Write-Both "project root: $root" 'Cyan'

# 1. Tamper Protection status (informational, it changes what is possible)
$tamper = $null
try {
    $tamper = (Get-MpComputerStatus).IsTamperProtected
    Write-Both ("Tamper Protection : {0}" -f $(if ($tamper) { 'ON  (scripted exclusion changes are usually ignored)' } else { 'off' })) `
        $(if ($tamper) { 'Yellow' } else { 'Gray' })
} catch {
    Write-Both "Tamper Protection : unknown ($($_.Exception.Message))" 'Yellow'
}

# 2. What we want excluded
$wantPaths = @($root)
$wantProcs = @('xmrig.exe', 'xmr-verify-proxy.exe')

# 3. Add them (ignore individual failures, we verify afterwards)
foreach ($p in $wantPaths) {
    try { Add-MpPreference -ExclusionPath $p -ErrorAction Stop } catch {
        Write-Both "add path FAILED: $p -> $($_.Exception.Message)" 'Red'
    }
}
foreach ($p in $wantProcs) {
    try { Add-MpPreference -ExclusionProcess $p -ErrorAction Stop } catch {
        Write-Both "add process FAILED: $p -> $($_.Exception.Message)" 'Red'
    }
}

# 4. Read back what is actually in effect
$effective = $null
try {
    $effective = Get-MpPreference
} catch {
    Write-Both "could not read back the exclusion list: $($_.Exception.Message)" 'Red'
}

$missing = @()
if ($effective) {
    $paths = @($effective.ExclusionPath)
    $procs = @($effective.ExclusionProcess)
    Write-Both ""
    Write-Both "verifying:" 'Cyan'
    foreach ($p in $wantPaths) {
        $ok = $paths -contains $p
        Write-Both ("  path    {0,-60} {1}" -f $p, $(if ($ok) { 'OK' } else { 'MISSING' })) `
            $(if ($ok) { 'Green' } else { 'Red' })
        if (-not $ok) { $missing += $p }
    }
    foreach ($p in $wantProcs) {
        $ok = $procs -contains $p
        Write-Both ("  process {0,-60} {1}" -f $p, $(if ($ok) { 'OK' } else { 'MISSING' })) `
            $(if ($ok) { 'Green' } else { 'Red' })
        if (-not $ok) { $missing += $p }
    }
}

if ($missing.Count -gt 0) {
    Write-Both ""
    Write-Both "$($missing.Count) exclusion(s) are NOT active." 'Red'
    if ($tamper) {
        Write-Both "Tamper Protection is ON, which blocks scripted changes. Use the UI:" 'Yellow'
    }
    Show-ManualSteps
    exit 1
}

Write-Both ""
Write-Both "all exclusions are active" 'Green'

# 5. Optional: fetch XMRig and prove it survives
if ($WithXmrig) {
    Write-Both ""
    Write-Both "fetching XMRig $XmrigVersion" 'Cyan'
    $dir = Join-Path $root 'testbench'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $zipName = "xmrig-$XmrigVersion-windows-x64.zip"
    $zip = Join-Path $dir $zipName
    $base = "https://github.com/xmrig/xmrig/releases/download/v$XmrigVersion"

    try {
        Invoke-WebRequest -Uri "$base/$zipName" -OutFile $zip -UseBasicParsing -TimeoutSec 300
        Invoke-WebRequest -Uri "$base/SHA256SUMS" -OutFile (Join-Path $dir 'SHA256SUMS') -UseBasicParsing -TimeoutSec 120
    } catch {
        Write-Both "download failed: $($_.Exception.Message)" 'Red'
        exit 1
    }

    $expected = (Select-String -Path (Join-Path $dir 'SHA256SUMS') -Pattern $zipName |
                 Select-Object -First 1).Line.Split(' ')[0]
    $actual = (Get-FileHash -Path $zip -Algorithm SHA256).Hash.ToLower()
    if ($expected -and ($actual -ne $expected.ToLower())) {
        Write-Both "SHA256 mismatch: expected $expected, got $actual" 'Red'
        exit 1
    }
    Write-Both "SHA256 OK: $actual" 'Green'

    Expand-Archive -Path $zip -DestinationPath $dir -Force
    $exe = Join-Path $dir "xmrig-$XmrigVersion\xmrig.exe"
    Start-Sleep -Milliseconds 500
    if (-not (Test-Path $exe)) {
        Write-Both "xmrig.exe disappeared right after extraction" 'Red'
        Show-ManualSteps
        Show-ThirdPartyAvHint
        exit 1
    }
    try {
        $version = & $exe --version 2>&1 | Select-Object -First 2
        Write-Both ("xmrig runs: {0}" -f ($version -join ' ')) 'Green'
    } catch {
        Write-Both "xmrig.exe still cannot be executed: $($_.Exception.Message)" 'Red'
        exit 1
    }
    if (-not (Test-Path $exe)) {
        Write-Both "xmrig.exe was deleted the moment it was executed." 'Red'
        Write-Both "Defender reports no detection for it, so another security product is doing this." 'Yellow'
        Show-ThirdPartyAvHint
        exit 1
    }
    Write-Both "XMRig is ready at: $exe" 'Green'
}

Write-Both ""
Write-Both "done" 'Green'
exit 0
