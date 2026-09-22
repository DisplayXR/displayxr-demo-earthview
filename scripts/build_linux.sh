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
#   CC / CXX         Compiler override. When unset, this script picks g++-12 or
#                    newer automatically (see the toolchain check below).
#
#   DXR_CMAKE_DIR    Where a provisioned CMake is unpacked (default
#                    /tmp/dxr-cmake). Set DXR_NO_CMAKE_DOWNLOAD=1 to refuse the
#                    download and fail with instructions instead.
#
# Toolchain floor (checked below — both are why a stock Ubuntu 22.04 box needs
# two extra things, and neither is optional):
#   * CMake >= 3.25    — cesium-native v0.61.0 picks its vcpkg triplet with
#                        `elseif(LINUX)`, and the LINUX variable only exists
#                        from CMake 3.25; an older cmake dies with "Cannot
#                        guess an appropriate value for VCPKG_TRIPLET".
#                        22.04 ships 3.22.1, so this script provisions a
#                        pinned Kitware CMake there. 24.04 (3.28) and 26.04
#                        are fine as shipped.
#   * GCC   >= 12      — cesium-native's ezvcpkg tree builds ada-url v3.3.0,
#                        which needs C++20 `constexpr std::string`; that is a
#                        GCC 12 libstdc++ feature, so 22.04's default GCC 11
#                        cannot build it. `sudo apt install g++-12` on 22.04;
#                        24.04 (GCC 13) and 26.04 (GCC 15) are fine as shipped.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# --- 0a. Toolchain floor --------------------------------------------------
# Resolve both floors HERE, rather than dying mid-configure on an unreadable
# cesium/vcpkg error 20 minutes in. On Ubuntu 22.04 neither is satisfied out of
# the box: cmake is provisioned automatically, g++-12 is one apt install.
CMAKE_MIN="3.25"
CMAKE_PIN="3.31.8"   # provisioned when the host cmake is older than CMAKE_MIN
GCC_MIN=12

cmake_ver() { "$1" --version 2>/dev/null | head -1 | awk '{print $3}'; }
cmake_ok()  { [ -n "$1" ] && [ "$(printf '%s\n%s\n' "$CMAKE_MIN" "$1" | sort -V | head -1)" = "$CMAKE_MIN" ]; }

CMAKE_VER=""
command -v cmake >/dev/null && CMAKE_VER="$(cmake_ver cmake)"
if ! cmake_ok "$CMAKE_VER"; then
    # Ubuntu 22.04's 3.22.1 lands here. cesium-native needs the CMake 3.25
    # `LINUX` variable to pick its vcpkg triplet, so this is not negotiable.
    DXR_CMAKE_DIR="${DXR_CMAKE_DIR:-/tmp/dxr-cmake}"
    PINNED="$DXR_CMAKE_DIR/cmake-$CMAKE_PIN-linux-x86_64/bin"
    if [ ! -x "$PINNED/cmake" ]; then
        if [ -n "${DXR_NO_CMAKE_DOWNLOAD:-}" ]; then
            echo "Error: cmake ${CMAKE_VER:-<none>} is older than the $CMAKE_MIN floor and" >&2
            echo "       DXR_NO_CMAKE_DOWNLOAD is set." >&2
            echo "       cesium-native v0.61.0 selects its vcpkg triplet with elseif(LINUX)," >&2
            echo "       and the LINUX variable only exists from CMake 3.25." >&2
            echo "       Install a newer CMake (https://apt.kitware.com), or unset" >&2
            echo "       DXR_NO_CMAKE_DOWNLOAD to let this script fetch $CMAKE_PIN." >&2
            exit 1
        fi
        echo "==> cmake ${CMAKE_VER:-<none>} < $CMAKE_MIN — provisioning CMake $CMAKE_PIN into $DXR_CMAKE_DIR"
        mkdir -p "$DXR_CMAKE_DIR"
        curl -fsSL "https://github.com/Kitware/CMake/releases/download/v$CMAKE_PIN/cmake-$CMAKE_PIN-linux-x86_64.tar.gz" \
            | tar -xz -C "$DXR_CMAKE_DIR"
    fi
    [ -x "$PINNED/cmake" ] || { echo "Error: failed to provision CMake $CMAKE_PIN." >&2; exit 1; }
    export PATH="$PINNED:$PATH"
    CMAKE_VER="$(cmake_ver cmake)"
fi

# Pick a compiler that can build cesium's ezvcpkg tree. An explicit CC/CXX from
# the caller (CI's matrix override) always wins and is never second-guessed.
gxx_major() { "$1" -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1; }
if [ -z "${CXX:-}" ]; then
    picked=""
    if command -v g++ >/dev/null && [ "$(gxx_major g++)" -ge "$GCC_MIN" ] 2>/dev/null; then
        picked="g++"
    else
        for v in 12 13 14 15 16; do
            if command -v "g++-$v" >/dev/null; then picked="g++-$v"; break; fi
        done
    fi
    if [ -z "$picked" ]; then
        echo "Error: no g++ >= $GCC_MIN found." >&2
        echo "       cesium-native's ezvcpkg tree builds ada-url v3.3.0, which needs" >&2
        echo "       C++20 constexpr std::string — a GCC $GCC_MIN libstdc++ feature." >&2
        echo "       On Ubuntu 22.04:  sudo apt install g++-12" >&2
        echo "       (24.04 and 26.04 ship a new enough GCC by default.)" >&2
        exit 1
    fi
    if [ "$picked" != "g++" ]; then
        export CXX="$picked"
        export CC="${picked/g++/gcc}"
        echo "==> Toolchain: default g++ is too old; using $CC / $CXX"
    fi
fi
echo "==> Toolchain: cmake $CMAKE_VER, $(${CXX:-g++} --version | head -1)"

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
