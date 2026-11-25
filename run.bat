@echo off
rem Convenience launcher for the packaged build.
rem Edit config.json (created from config.example.json on first run) first:
rem   listen   - where miners connect, e.g. 0.0.0.0:3333
rem   upstream - the pool, e.g. xmr.kryptex.network:7029
setlocal
cd /d "%~dp0"

if not exist config.json (
    echo config.json not found - creating one from config.example.json
    copy /y config.example.json config.json >nul
    echo Edit config.json to set your pool, then run this file again.
    echo.
)

echo Starting xmr-verify-proxy
echo   listen / upstream / stats are printed below
echo   stop with Ctrl+C
echo.
xmr-verify-proxy.exe run --config config.json
echo.
echo proxy exited with code %errorlevel%
pause
