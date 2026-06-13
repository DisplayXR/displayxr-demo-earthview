@echo off
setlocal EnableDelayedExpansion
REM ============================================================================
REM run_windows_dev.bat — launch the locally-built EarthView against a DisplayXR
REM Windows runtime: the runtime repo's dev build if present (preferred for M2
REM iteration), else the installed runtime.
REM
REM Mirrors scripts\run_macos_dev.sh:
REM   - sources a gitignored .env.local (repo root) so GOOGLE_MAPS_API_KEY is set
REM     without exporting it by hand. .env.local is the dev key store on this
REM     machine; it is never committed (.gitignore) and never staged into the
REM     installer. (The distributed app gets its key from the user — see
REM     docs/api-key.md.)
REM   - points XR_RUNTIME_JSON at the dev runtime so the loader picks it up.
REM
REM IMPORTANT: run from a NON-ELEVATED terminal. The bundled Khronos
REM openxr_loader.dll silently ignores XR_RUNTIME_JSON in elevated/admin
REM processes and falls back to HKLM ActiveRuntime (see runtime CLAUDE.md).
REM
REM Usage:
REM   scripts\build_windows.bat
REM   scripts\run_windows_dev.bat [extra args passed to the exe]
REM Override the runtime with: set DISPLAYXR_RUNTIME_JSON=C:\path\to\DisplayXR_win64.json
REM ============================================================================

set "REPO_ROOT=%~dp0.."
for %%I in ("%REPO_ROOT%") do set "REPO_ROOT=%%~fI"

REM --- API key for local dev: load .env.local (KEY=VALUE per line) -------------
set "ENV_LOCAL=%REPO_ROOT%\.env.local"
if exist "%ENV_LOCAL%" (
    for /f "usebackq eol=# tokens=1,* delims==" %%A in ("%ENV_LOCAL%") do (
        set "_k=%%A"
        REM Trim surrounding spaces on the key.
        for /f "tokens=* delims= " %%K in ("!_k!") do set "_k=%%K"
        if /i "!_k!"=="GOOGLE_MAPS_API_KEY" set "GOOGLE_MAPS_API_KEY=%%B"
    )
    if defined GOOGLE_MAPS_API_KEY (
        echo ==^> Loaded GOOGLE_MAPS_API_KEY from .env.local
    )
) else (
    echo ==^> No .env.local found; relying on an existing GOOGLE_MAPS_API_KEY or earthview.ini
)

REM --- Locate a runtime: explicit override, then dev build, then installed -----
set "RT_JSON="
if defined DISPLAYXR_RUNTIME_JSON (
    if exist "%DISPLAYXR_RUNTIME_JSON%" set "RT_JSON=%DISPLAYXR_RUNTIME_JSON%"
)
if not defined RT_JSON (
    set "DEV_RT=%REPO_ROOT%\..\openxr-3d-display\_package\DisplayXR_win64.json"
    if exist "!DEV_RT!" for %%I in ("!DEV_RT!") do set "RT_JSON=%%~fI"
)
if not defined RT_JSON (
    set "INST_RT=%ProgramFiles%\DisplayXR\Runtime\DisplayXR_win64.json"
    if exist "!INST_RT!" set "RT_JSON=!INST_RT!"
)
if defined RT_JSON (
    set "XR_RUNTIME_JSON=!RT_JSON!"
    echo ==^> Runtime: !RT_JSON!
) else (
    echo ==^> No dev runtime JSON found; falling back to HKLM ActiveRuntime ^(installed runtime^)
)

REM --- Launch -----------------------------------------------------------------
set "BIN=%REPO_ROOT%\build\windows\earthview_handle_vk_win.exe"
if not exist "%BIN%" (
    echo ERROR: %BIN% not found — build first: scripts\build_windows.bat
    exit /b 1
)
echo ==^> Launching %BIN% %*
"%BIN%" %*
