# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Standalone build — lives inside displayxr-demo-earthview repo as
# macos/CMakeLists.txt. Assumes sibling dirs: ../common, ../tiles_common,
# ../openxr_includes. Top-level CMakeLists.txt is expected to have already
# added ../common and ../tiles_common as subdirectories, exposing the
# `sr_common_base` and `tile_renderer` targets.
#
# macos/main.mm is the macOS entry point: Cocoa window + MoltenVK + OpenXR
# session + HUD/input, with the renderer call sites wired to the EarthView
# tile engine (cesium-native streaming of Google Photorealistic 3D Tiles).

cmake_minimum_required(VERSION 3.25)

if(NOT APPLE)
    return()
endif()

find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR loader — system install (brew), find_package, or user-provided root.
set(OPENXR_FOUND FALSE)
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
endif()

if(NOT OPENXR_FOUND)
    find_library(OPENXR_LOADER_LIB NAMES openxr_loader)
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        set_target_properties(OpenXR::openxr_loader PROPERTIES
            IMPORTED_LOCATION "${OPENXR_LOADER_LIB}"
        )
        message(STATUS "Found OpenXR loader: ${OPENXR_LOADER_LIB}")
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Install via: brew install openxr-loader")
endif()

add_executable(earthview_handle_vk_macos
    main.mm
)

target_include_directories(earthview_handle_vk_macos PRIVATE
    ${CMAKE_SOURCE_DIR}/openxr_includes
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/tiles_common
)

target_link_libraries(earthview_handle_vk_macos PRIVATE
    sr_common_base
    tile_renderer
    OpenXR::openxr_loader
    Vulkan::Vulkan
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework CoreText"
    "-framework CoreFoundation"
    "-framework UniformTypeIdentifiers"
)

# Copy the Google attribution logo next to the exe (HUD strip asset — policy).
set(GOOGLE_LOGO "${CMAKE_CURRENT_SOURCE_DIR}/assets/google_logo.png")
if(EXISTS "${GOOGLE_LOGO}")
    add_custom_command(TARGET earthview_handle_vk_macos POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOOGLE_LOGO}" "$<TARGET_FILE_DIR:earthview_handle_vk_macos>/google_logo.png"
        COMMENT "Copying Google attribution logo next to exe"
    )
endif()
