#!/usr/bin/env bash
#
# scripts/build_linux.sh — Build the Linux EarthView binary (build-green,
# issue #19; M8 Linux epic runtime#699).
#
# Mirrors scripts/build_macos.sh + the runtime repo's
# docs/guides/linux-demo-port.md, with the Linux swaps: system Vulkan
# (libvulkan-dev — no MoltenVK, no ICD manifest), a from-source OpenXR loader
# pinned to release-1.1.43 (kept EQUAL to linux/CMakeLists.txt's FetchContent
# fallback GIT_TAG — CI runs this script, so the CI pin follows), and no
# installer step (Linux packaging is out of scope until on-screen lands).
#
# EarthView streams Google Photorealistic 3D Tiles via cesium-native, so this
# script also clones cesium-native (v0.61.0) when absent — the same clone the
# macOS/Windows CI does. cesium bootstraps its own dependency tree (Draco, KTX,
# TLS/curl, …) through ezvcpkg on first configure (~11 min cold, cached after).
#
# Usage:
#   ./scripts/build_linux.sh
#
# Deps (Ubuntu): see .github/workflows/build-linux.yml. curl/TLS + the X11 /
# Wayland / ALSA headers ezvcpkg + its ports need to build from source.
#
# Env:
#   OPENXR_VERSION   OpenXR-SDK release tag for the loader (default 1.1.43).
#                    Keep this pin equal to linux/CMakeLists.txt's FetchContent
#                    GIT_TAG.
#   EZVCPKG_BASEDIR  Where cesium's ezvcpkg installs its dependency tree
#                    (default: the ezvcpkg built-in under $HOME). CI pins it to
#                    a fixed, cacheable path.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# --- 0. Clone cesium-native (v0.61.0) if absent ---------------------------
# Consumed via add_subdirectory(third_party/cesium-native) from the root
# CMakeLists. Gitignored (not vendored) — clone per README.
if [ ! -f "third_party/cesium-native/CMakeLists.txt" ]; then
    echo "==> Cloning cesium-native v0.61.0 -> third_party/cesium-native"
    rm -rf third_party/cesium-native
    git clone --branch v0.61.0 --depth 1 --recurse-submodules \
        https://github.com/CesiumGS/cesium-native.git third_party/cesium-native
else
    echo "==> cesium-native present at third_party/cesium-native"
fi

# --- 1. Build OpenXR loader from source -----------------------------------
# Distro loaders lag; build the pinned Khronos loader and install it under
# /tmp/openxr-install (mirrors build_macos.sh + the runtime repo's
# scripts/build_linux.sh --apps). Cached: skipped if both the .so and the
# CMake package config are already present.
OPENXR_VERSION="${OPENXR_VERSION:-1.1.43}"
OPENXR_DIR="/tmp/openxr-install"
if [ ! -f "$OPENXR_DIR/lib/libopenxr_loader.so" ] || \
   [ ! -f "$OPENXR_DIR/lib/cmake/openxr/OpenXRConfig.cmake" ]; then
    echo "==> Building OpenXR loader $OPENXR_VERSION -> $OPENXR_DIR"
    rm -rf /tmp/openxr-sdk "$OPENXR_DIR"
    git clone --depth 1 --branch "release-$OPENXR_VERSION" \
        https://github.com/KhronosGroup/OpenXR-SDK-Source.git /tmp/openxr-sdk
    cmake -B /tmp/openxr-sdk/build -S /tmp/openxr-sdk -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$OPENXR_DIR" \
        -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
        -DBUILD_WITH_SYSTEM_JSONCPP=OFF
    cmake --build /tmp/openxr-sdk/build
    cmake --install /tmp/openxr-sdk/build
else
    echo "==> OpenXR loader cached at $OPENXR_DIR"
fi

# --- 2. cmake build -------------------------------------------------------
# Vulkan via system libvulkan-dev; the OpenXR loader via CMAKE_PREFIX_PATH.
# Disable the compiler-launcher cache explicitly (empty) — cesium-native
# auto-wires sccache if it finds it on PATH, and an sccache backed by the
# intermittently-unavailable GitHub Actions cache can hard-fail the compile.
LAUNCHER="${CMAKE_CXX_COMPILER_LAUNCHER:-}"
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$OPENXR_DIR" \
    -DCMAKE_C_COMPILER_LAUNCHER="$LAUNCHER" \
    -DCMAKE_CXX_COMPILER_LAUNCHER="$LAUNCHER"
cmake --build build

BIN="$REPO_ROOT/build/linux/earthview_handle_vk_linux"
[ -x "$BIN" ] || { echo "Error: expected binary not found at $BIN" >&2; exit 1; }

echo ""
echo "Built: $BIN"
echo "Run against a dev runtime: scripts/run_earthview_linux.sh"
