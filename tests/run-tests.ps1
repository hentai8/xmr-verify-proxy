# Automated regression tests for xmr-verify-proxy.
#
# Everything here runs locally and offline: the job-side and share-side checks
# are driven by the built-in mock pool, so no real pool and no real money is
# involved. Only `selftest` needs the captured share vectors in tests/data.
#
# usage:
#   powershell -ExecutionPolicy Bypass -File .\tests\run-tests.ps1
#   powershell -ExecutionPolicy Bypass -File .\tests\run-tests.ps1 -Exe .\build\Release\xmr-verify-proxy.exe
#
# Exit code 0 = all checks passed.

param(
    [string]$Exe,
    [int]$ProxyPort = 13333,
    [int]$StatsPort = 13356,
    [int]$MockPort = 17029
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$logDir = Join-Path $root 'test-logs'
New-Item -ItemType Directory -Force $logDir | Out-Null

if (-not $Exe) {
    # Prefer the most recent build; fall back to the packaged copy.
    foreach ($candidate in @(
            (Join-Path $root 'build\Release\xmr-verify-proxy.exe'),
            (Join-Path $root 'dist\xmr-verify-proxy.exe'))) {
        if (Test-Path $candidate) { $Exe = $candidate; break }
    }
}
if (-not $Exe -or -not (Test-Path $Exe)) {
    Write-Host "binary not found - build it first:  powershell -ExecutionPolicy Bypass -File .\build.ps1" -ForegroundColor Red
    exit 2
}
$Exe = (Resolve-Path $Exe).Path
Write-Host "binary: $Exe" -ForegroundColor Cyan

$script:passed = 0
$script:failed = 0

function Check {
    param([bool]$Condition, [string]$Message)
    if ($Condition) {
        Write-Host "  PASS  $Message" -ForegroundColor Green
        $script:passed++
    } else {
        Write-Host "  FAIL  $Message" -ForegroundColor Red
        $script:failed++
    }
}

function Wait-Port {
    param([int]$Port, [int]$TimeoutSec = 20)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $c = New-Object Net.Sockets.TcpClient
            $c.Connect('127.0.0.1', $Port)
            $c.Close()
            return $true
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    return $false
}

function Stop-Tree {
    param($Proc)
    if ($Proc -and -not $Proc.HasExited) {
        Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 300
    }
}

# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "[1/3] RandomX correctness against 9 real accepted shares" -ForegroundColor Cyan
$vectors = Join-Path $root 'tests\data\submit-vectors.json'
$out = & $Exe selftest --vectors $vectors 2>&1 | Out-String
Check ($out -match '9 passed, 0 failed') 'all 9 captured shares recompute to the recorded result'
if ($out -notmatch '9 passed, 0 failed') { Write-Host $out }

# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "[2/3] job validation: 4 malformed pool jobs must never reach the miner" -ForegroundColor Cyan
$mockOut = Join-Path $logDir 'mock-job.out'
$proxyOut = Join-Path $logDir 'proxy-job.out'
$mock = Start-Process -FilePath $Exe -ArgumentList 'mockpool', '--mock-listen', "127.0.0.1:$MockPort", '--mode', 'all' `
    -RedirectStandardOutput $mockOut -RedirectStandardError "$logDir\mock-job.err" -PassThru -WindowStyle Hidden
$proxy = Start-Process -FilePath $Exe -ArgumentList 'run', '--upstream', "127.0.0.1:$MockPort", `
    '--listen', "127.0.0.1:$ProxyPort" -RedirectStandardOutput $proxyOut `
    -RedirectStandardError "$logDir\proxy-job.err" -PassThru -WindowStyle Hidden
try {
    Check (Wait-Port $ProxyPort) "proxy listening on $ProxyPort"
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) {
        $trace = & python (Join-Path $root 'scripts\trace_client.py') --target "127.0.0.1:$ProxyPort" `
            --login '4TESTADDRESSFORPROXYTESTSONLY/worker' --seconds 9 2>&1 | Out-String
        Check ($trace -match 'goodjob00005') 'the valid job after the broken ones is delivered'
        Check (-not ($trace -match 'badblob00001')) 'blob with wrong length is dropped'
        Check (-not ($trace -match 'badseed00002')) 'job without seed_hash is dropped'
        Check (-not ($trace -match 'badalgo00003')) 'job for an unverifiable algo is dropped'
        Check (-not ($trace -match 'badtarget004')) 'job with a zero target is dropped'
    } else {
        Write-Host "  SKIP  python not found, skipping the protocol trace checks" -ForegroundColor Yellow
    }
} finally {
    Stop-Tree $proxy
    Stop-Tree $mock
}

# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "[3/3] share validation: bad shares blocked locally, good share forwarded" -ForegroundColor Cyan
$mockOut2 = Join-Path $logDir 'mock-shares.out'
$proxyOut2 = Join-Path $logDir 'proxy-shares.out'
$mock2 = Start-Process -FilePath $Exe -ArgumentList 'mockpool', '--mock-listen', "127.0.0.1:$MockPort", `
    '--mode', 'valid', '--easy' -RedirectStandardOutput $mockOut2 `
    -RedirectStandardError "$logDir\mock-shares.err" -PassThru -WindowStyle Hidden
$proxy2 = Start-Process -FilePath $Exe -ArgumentList 'run', '--upstream', "127.0.0.1:$MockPort", `
    '--listen', "127.0.0.1:$ProxyPort" -RedirectStandardOutput $proxyOut2 `
    -RedirectStandardError "$logDir\proxy-shares.err" -PassThru -WindowStyle Hidden
try {
    Check (Wait-Port $ProxyPort) "proxy listening on $ProxyPort"
    $mine = & $Exe mine --target "127.0.0.1:$ProxyPort" --login '4TESTADDRESSFORPROXYTESTSONLY/worker' `
        --fault all --threads 4 --timeout 180 2>&1 | Out-String
    foreach ($case in @(
            'bad hash \(result != RandomX',
            'low difficulty share',
            'unknown/stale job_id',
            'valid share',
            'duplicate of an accepted share')) {
        Check ($mine -match "\[PASS\] $case") "case: $($case -replace '\\', '')"
    }
    Check ($mine -match 'ALL EXPECTATIONS MET') 'test miner reports no failures'
    if ($mine -notmatch 'ALL EXPECTATIONS MET') { Write-Host $mine }
} finally {
    Stop-Tree $proxy2
    Stop-Tree $mock2
}

# ---------------------------------------------------------------------------
Write-Host ""
if ($script:failed -eq 0) {
    Write-Host "ALL $($script:passed) CHECKS PASSED" -ForegroundColor Green
    exit 0
}
Write-Host "$($script:failed) of $($script:passed + $script:failed) checks FAILED" -ForegroundColor Red
Write-Host "logs: $logDir" -ForegroundColor Yellow
exit 1
