# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Native build for the Android leg of displayxr-demo-earthview (M3 bring-up).
# Reuses the cube_handle_vk_android OpenXR-Android harness pattern (loader →
# instance → Vulkan device → stereo atlas swapchains → session) and binds to
# the DisplayXR runtime out-of-process (ADR-025).
#
# THIS IS THE PLACEHOLDER LEG: main.cpp brings up the OOP session and clears the
# swapchain to a globe-blue frame to prove the harness on NP02J. The cesium-
# native tile streaming + the shared tiles_common/ renderer + the focus/orbit
# camera are wired in the NEXT step (see docs/m3-android-plan.md §4). At that
# point this file gains `add_subdirectory(${REPO_ROOT}/third_party/cesium-native)`
# + `add_subdirectory(${REPO_ROOT}/tiles_common)` and links `tile_renderer`
# (the §5a spike proved the cesium dep tree builds for arm64-android).

cmake_minimum_required(VERSION 3.22.1)
project(earthview_vk_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root is four levels up from android/src/main/cpp.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

# ── NativeActivity glue (ships with the NDK) ─────────────────────────────
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# Khronos OpenXR loader from the openxr_loader_for_android .aar (prefab) —
# defines OpenXR::openxr_loader with libopenxr_loader.so + headers.
find_package(OpenXR REQUIRED CONFIG)

add_library(earthview_vk_android SHARED main.cpp)

target_include_directories(earthview_vk_android BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    # DisplayXR's in-tree OpenXR headers (XR_EXT_display_info / XR_EXT_view_rig)
    # BEFORE the AAR's stock headers so <openxr/...> resolves to our set.
    ${REPO_ROOT}/openxr_includes)

set_target_properties(earthview_vk_android PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(earthview_vk_android
    native_app_glue
    OpenXR::openxr_loader
    vulkan
    android
    log)
