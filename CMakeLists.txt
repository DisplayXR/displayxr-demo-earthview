# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Top-level CMakeLists.txt for the displayxr-demo-earthview standalone repo.
# Orchestrates the shared `common` and `tiles_common` libraries plus the
# cesium-native streaming engine, then dispatches to macos/ or windows/ for
# the platform-specific executable. (android/ is M3.)
#
# The headless M0 spike (spike/) is a standalone project and is deliberately
# NOT added here — build it from spike/ directly (see spike/CMakeLists.txt).

cmake_minimum_required(VERSION 3.25)
project(displayxr-demo-earthview LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# cesium-native pinned at v0.61.0 (clone per README; ezvcpkg bootstraps deps —
# first configure downloads/builds Draco/KTX/TLS, ~11 min cold, cached after).
set(CESIUM_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cesium-native
    cesium-native EXCLUDE_FROM_ALL)

# Shared libraries (built once, linked by the platform executable).
add_subdirectory(common)
add_subdirectory(tiles_common)

# Platform-specific executable.
if(APPLE)
    add_subdirectory(macos)
elseif(WIN32)
    add_subdirectory(windows)
else()
    message(FATAL_ERROR "displayxr-demo-earthview supports macOS and Windows only (Android = M3).")
endif()
