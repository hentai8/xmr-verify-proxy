# Builds xmr-verify-proxy on Windows.
#
# Prerequisites:
#   - Visual Studio 2022 Build Tools with the "Desktop development with C++" workload
#     (needs cl.exe and ml64.exe; the RandomX JIT assembler is compiled with MASM)
#   - CMake:  python -m pip install cmake     (or any CMake on PATH >= 3.20)
#
# usage:
#   powershell -ExecutionPolicy Bypass -File .\build.ps1
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 -Avx2     # /arch:AVX2 for RandomX
#
# The finished binary lands in dist\xmr-verify-proxy.exe

param(
    [switch]$Avx2,
    [string]$Config = "Release"
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $py = Get-Command python -ErrorAction SilentlyContinue
    if ($py) {
        $path = & python -c "import cmake,os;print(os.path.join(os.path.dirname(cmake.__file__),'data','bin','cmake.exe'))" 2>$null
        if ($path -and (Test-Path $path)) { return $path.Trim() }
    }
    throw "cmake not found. Install it with:  python -m pip install cmake"
}

function Find-VcVars {
    $candidates = @(
        "$env:ProgramFiles\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    return $null
}

$cmake = Find-CMake
$vcvars = Find-VcVars
if (-not $vcvars) {
    Write-Host "warning: could not find vcvars64.bat. Make sure VS 2022 C++ Build Tools are installed." -ForegroundColor Yellow
}

$buildDir = if ($Avx2) { Join-Path $root 'build-avx2' } else { Join-Path $root 'build' }
$cmakeArgs = @('-S', $root, '-B', $buildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64')
if ($Avx2) { $cmakeArgs += '-DXVP_AVX2=ON' }

Write-Host "== configuring ($buildDir) ==" -ForegroundColor Cyan
& $cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "== building ($Config) ==" -ForegroundColor Cyan
& $cmake --build $buildDir --config $Config --target xmr-verify-proxy -- /m
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$exe = Join-Path $buildDir "$Config\xmr-verify-proxy.exe"
if (-not (Test-Path $exe)) { throw "expected binary not found: $exe" }

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item $exe (Join-Path $dist 'xmr-verify-proxy.exe') -Force
Copy-Item (Join-Path $root 'config.example.json') (Join-Path $dist 'config.example.json') -Force
Copy-Item (Join-Path $root 'README.md') (Join-Path $dist 'README.md') -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $root 'run.bat') (Join-Path $dist 'run.bat') -Force

Write-Host ""
Write-Host "== done ==" -ForegroundColor Green
Get-Item $exe | Select-Object FullName, Length, LastWriteTime | Format-List
Write-Host "packaged: dist\xmr-verify-proxy.exe" -ForegroundColor Green
Write-Host ""
Write-Host "self-check:  .\dist\xmr-verify-proxy.exe selftest --vectors tests\data\submit-vectors.json"
Write-Host "run:         .\dist\run.bat        (or: .\dist\xmr-verify-proxy.exe run --config config.json)"
