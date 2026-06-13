@echo off
setlocal
REM ============================================================================
REM build_windows.bat — canonical one-shot Windows build for EarthView.
REM Sets up the VS 2022 toolchain + Ninja, then configures + builds. Unlike
REM modelviewer this repo pulls cesium-native (add_subdirectory + ezvcpkg), so
REM the first configure downloads/builds Draco/KTX/TLS (~11 min cold, cached
REM after) and the deps (OpenXR loader + Vulkan SDK) must be discoverable.
REM
REM Prereq (one-time): clone cesium-native into third_party/ —
REM   git clone --branch v0.61.0 --recurse-submodules ^
REM     https://github.com/CesiumGS/cesium-native.git third_party\cesium-native
REM
REM Override dep roots with OpenXR_ROOT / VULKAN_SDK_PREFIX before calling.
REM ============================================================================
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: VS 2022 not found
    exit /b 1
)
set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe;%PATH%"
cd /d "%~dp0\.."

if not exist "third_party\cesium-native\CMakeLists.txt" (
    echo ERROR: third_party\cesium-native missing. Clone it first:
    echo   git clone --branch v0.61.0 --recurse-submodules https://github.com/CesiumGS/cesium-native.git third_party\cesium-native
    exit /b 1
)

if "%OpenXR_ROOT%"=="" set "OpenXR_ROOT=C:/dev/openxr_sdk"
if "%VULKAN_SDK_PREFIX%"=="" set "VULKAN_SDK_PREFIX=C:/VulkanSDK/1.4.341.1"

echo === Configuring ===
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT=%OpenXR_ROOT% -DCMAKE_PREFIX_PATH=%VULKAN_SDK_PREFIX% || goto :error
echo === Building ===
cmake --build build || goto :error
echo.
echo === ALL DONE ===
echo Run: scripts\run_windows_dev.bat   ^(or build\windows\earthview_handle_vk_win.exe^)
exit /b 0
:error
echo === BUILD FAILED ===
exit /b 1
