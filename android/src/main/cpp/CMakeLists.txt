# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Native build for the Android leg of displayxr-demo-earthview (M3).
# Reuses the cube_handle_vk_android OpenXR-Android harness (loader → instance →
# Vulkan2 device → atlas swapchain → session), binds to the DisplayXR runtime
# out-of-process (ADR-025), and now compiles the SHARED tiles_common/ Vulkan
# tile renderer + cesium-native streaming engine (the §5a spike proved the
# cesium dep tree builds for arm64-android — see docs/m3-android-plan.md).

cmake_minimum_required(VERSION 3.22.1)
project(earthview_vk_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)          # cesium-native + tiles_common require C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root is four levels up from android/src/main/cpp.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

# ── cesium-native deps via ezvcpkg, cross-built for arm64-android ─────────
# ezvcpkg installs the dep tree (OpenSSL/curl/draco/KTX/…) using VCPKG_TRIPLET;
# the §5a spike proved arm64-android builds cleanly. Two wiring requirements
# the spike surfaced:
#   1. VCPKG_TRIPLET must be arm64-android (else ezvcpkg defaults to the host
#      triplet). Passed from gradle (-DVCPKG_TRIPLET) with a fallback here.
#   2. vcpkg's android triplet + ezvcpkg's chainload need ANDROID_NDK_HOME; AGP
#      gives us ${ANDROID_NDK}, so bridge it into the environment.
if(NOT DEFINED ENV{ANDROID_NDK_HOME})
    set(ENV{ANDROID_NDK_HOME} "${ANDROID_NDK}")
endif()
if(NOT DEFINED ENV{ANDROID_NDK_ROOT})
    set(ENV{ANDROID_NDK_ROOT} "${ANDROID_NDK}")
endif()
if(NOT VCPKG_TRIPLET)
    set(VCPKG_TRIPLET "arm64-android" CACHE STRING "ezvcpkg triplet" FORCE)
endif()
# ezvcpkg overwrites CMAKE_TOOLCHAIN_FILE with vcpkg's; chainload the NDK
# toolchain so cesium's own targets still compile for arm64-v8a.
if(NOT VCPKG_CHAINLOAD_TOOLCHAIN_FILE)
    set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
        "${ANDROID_NDK}/build/cmake/android.toolchain.cmake" CACHE STRING "" FORCE)
endif()

set(CESIUM_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
add_subdirectory(${REPO_ROOT}/third_party/cesium-native cesium-native EXCLUDE_FROM_ALL)

# ── Shared scene layer (tile_renderer: cesium glue + Vulkan draw + geo math) ─
# Reuses the maintained tiles_common/CMakeLists.txt — same renderer the desktop
# legs build. It bakes the tile.vert/tile.frag SPIR-V headers and links cesium.
add_subdirectory(${REPO_ROOT}/tiles_common tiles_common)

# ── NativeActivity glue (ships with the NDK) ─────────────────────────────
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# Khronos OpenXR loader from the openxr_loader_for_android .aar (prefab).
find_package(OpenXR REQUIRED CONFIG)

add_library(earthview_vk_android SHARED main.cpp)

target_include_directories(earthview_vk_android BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${REPO_ROOT}/tiles_common               # geo_math.h, tile_engine.h, tile_renderer.h
    ${REPO_ROOT}/common                     # leia_math.h
    # DisplayXR's in-tree OpenXR headers BEFORE the AAR's stock set.
    ${REPO_ROOT}/openxr_includes)

set_target_properties(earthview_vk_android PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(earthview_vk_android
    tile_renderer
    native_app_glue
    OpenXR::openxr_loader
    vulkan
    android
    log)
