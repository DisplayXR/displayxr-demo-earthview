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

REM --- Vulkan SDK ---------------------------------------------------------------
REM Default to the VULKAN_SDK env var the LunarG installer exports (any version);
REM find_package(Vulkan) picks it up, so no -DCMAKE_PREFIX_PATH is needed. An
REM explicit VULKAN_SDK_PREFIX override still wins (passed as CMAKE_PREFIX_PATH).
set "VK_PREFIX_ARG="
if not "%VULKAN_SDK_PREFIX%"=="" set "VK_PREFIX_ARG=-DCMAKE_PREFIX_PATH=%VULKAN_SDK_PREFIX%"
if "%VULKAN_SDK_PREFIX%"=="" if "%VULKAN_SDK%"=="" (
    echo ERROR: neither VULKAN_SDK nor VULKAN_SDK_PREFIX is set. Install the Vulkan
    echo        SDK from https://vulkan.lunarg.com and open a fresh terminal so
    echo        VULKAN_SDK is exported, or set VULKAN_SDK_PREFIX, then re-run.
    goto :error
)

REM --- glslangValidator on PATH ------------------------------------------------
REM Put the SDK's Bin on PATH so CMake's find_program(glslangValidator) resolves
REM (the bare find_program searches PATH only; some installs set the SDK env
REM without adding Bin to PATH, e.g. the winget Vulkan SDK package).
if not "%VULKAN_SDK%"=="" set "PATH=%VULKAN_SDK%\Bin;%PATH%"
if not "%VULKAN_SDK_PREFIX%"=="" set "PATH=%VULKAN_SDK_PREFIX%\Bin;%PATH%"

REM --- OpenXR loader -------------------------------------------------------------
REM If OpenXR_ROOT isn't overridden, auto-provision the prebuilt Khronos loader,
REM pinned to the same spec revision as the vendored openxr_includes/ headers
REM (XR_CURRENT_API_VERSION = 1.1.51), into build\openxr_sdk so a fresh clone
REM builds with no manually-staged SDK. (Mirrors .github/workflows/build-windows.yml.)
set "OPENXR_VER=1.1.51"
if "%OpenXR_ROOT%"=="" set "OpenXR_ROOT=%CD%/build/openxr_sdk"
if not exist "build\openxr_sdk\x64\lib\openxr_loader.lib" if "%OpenXR_ROOT%"=="%CD%/build/openxr_sdk" (
    echo === Provisioning OpenXR loader %OPENXR_VER% ===
    if not exist build mkdir build
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "$ErrorActionPreference='Stop';" ^
      "$u='https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-%OPENXR_VER%/openxr_loader_windows-%OPENXR_VER%.zip';" ^
      "Invoke-WebRequest -Uri $u -OutFile 'build\openxr_loader.zip';" ^
      "Expand-Archive -Path 'build\openxr_loader.zip' -DestinationPath 'build\openxr_sdk' -Force;" ^
      "Remove-Item 'build\openxr_loader.zip' -Force" || goto :error
)

echo === Configuring ===
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OpenXR_ROOT%" %VK_PREFIX_ARG% || goto :error
echo === Building ===
cmake --build build || goto :error
echo.
echo === ALL DONE ===
echo Run: scripts\run_windows_dev.bat   ^(or build\windows\earthview_handle_vk_win.exe^)
exit /b 0
:error
echo === BUILD FAILED ===
exit /b 1
