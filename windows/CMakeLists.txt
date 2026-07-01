# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Standalone build — lives inside displayxr-demo-earthview repo as
# windows/CMakeLists.txt. Assumes sibling dirs: ../common, ../tiles_common,
# ../openxr_includes. Top-level CMakeLists.txt is expected to have already
# added ../common and ../tiles_common as subdirectories (and cesium-native),
# exposing the `sr_common_base`, `sr_common`, and `tile_renderer` targets.
#
# windows/main.cpp + xr_session.cpp originated from displayxr-demo-modelviewer
# (which in turn came from displayxr-demo-gaussiansplat); the window / OpenXR /
# HUD / input glue is reused as-is and the renderer call sites are retargeted
# to tiles_common (TileEngine + TileRenderer — cesium-native streaming of
# Google Photorealistic 3D Tiles), mirroring what macos/main.mm does. See
# docs/roadmap.md (M2) and docs/rendering-notes.md.

cmake_minimum_required(VERSION 3.25)

if(NOT WIN32)
    return()
endif()

if(MSVC)
    # cesium-native is built with vcpkg's x64-windows-static-md triplet = the
    # DYNAMIC CRT (/MD). Match it (the modelviewer used /MT, but linking cesium
    # forces /MD), else LNK2038 RuntimeLibrary mismatch.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(STATUS "Vulkan SDK not found — skipping earthview_handle_vk_win")
    return()
endif()
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR loader — CONFIG pkg first, then a user-specified OpenXR_ROOT fallback.
set(OPENXR_FOUND FALSE)
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
    message(STATUS "Found OpenXR via find_package")
endif()

if(NOT OPENXR_FOUND AND DEFINED OpenXR_ROOT)
    find_library(OPENXR_LOADER_LIB
        NAMES openxr_loader openxr_loader-1_0
        PATHS "${OpenXR_ROOT}/x64/lib" "${OpenXR_ROOT}/lib" "${OpenXR_ROOT}/bin"
        NO_DEFAULT_PATH
    )
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        find_file(OPENXR_LOADER_DLL
            NAMES openxr_loader.dll openxr_loader-1_0.dll
            PATHS "${OpenXR_ROOT}/x64/bin" "${OpenXR_ROOT}/bin"
            NO_DEFAULT_PATH
        )
        if(OPENXR_LOADER_DLL)
            set_target_properties(OpenXR::openxr_loader PROPERTIES
                IMPORTED_LOCATION "${OPENXR_LOADER_DLL}"
                IMPORTED_IMPLIB "${OPENXR_LOADER_LIB}"
            )
        else()
            set_target_properties(OpenXR::openxr_loader PROPERTIES
                IMPORTED_IMPLIB "${OPENXR_LOADER_LIB}"
            )
        endif()
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Provide -DOpenXR_ROOT=<path> or install via vcpkg.")
endif()

add_executable(earthview_handle_vk_win WIN32
    main.cpp
    xr_session.cpp
    xr_session.h
    resource.rc
)

target_include_directories(earthview_handle_vk_win PRIVATE
    ${CMAKE_SOURCE_DIR}/openxr_includes
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/tiles_common
)

target_link_libraries(earthview_handle_vk_win PRIVATE
    sr_common_base
    sr_common
    tile_renderer
    OpenXR::openxr_loader
    Vulkan::Vulkan
    d3d11
    d2d1
    dwrite
    comdlg32
)

if(MSVC)
    # /utf-8: tell MSVC the source files are UTF-8 so multi-byte glyphs like
    # "…" in HUD button labels aren't mangled at compile time.
    target_compile_options(earthview_handle_vk_win PRIVATE /utf-8)

    file(RELATIVE_PATH _manifest_rel
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/earthview_handle_vk_win.manifest")
    set_target_properties(earthview_handle_vk_win PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${_manifest_rel}"
    )
endif()

# Copy OpenXR loader DLL next to exe. Use $<TARGET_FILE:...> rather than the
# IMPORTED_LOCATION property: the Khronos OpenXR CONFIG package only sets the
# *config-specific* IMPORTED_LOCATION_<CONFIG>, leaving the generic
# IMPORTED_LOCATION unset, which would silently skip the copy.
if(TARGET OpenXR::openxr_loader)
    add_custom_command(TARGET earthview_handle_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:OpenXR::openxr_loader>"
            "$<TARGET_FILE_DIR:earthview_handle_vk_win>"
        COMMENT "Copying OpenXR loader DLL"
    )
endif()

# Vendor weaver DLLs (e.g. SimulatedRealityVulkanBeta.dll) are intentionally
# NOT bundled — they ship with the vendor plug-in installer and must not
# appear in a vendor-neutral DisplayXR demo.

# Copy the Google attribution logo next to the exe (HUD strip asset — Map
# Tiles API policy, PRD §7.3). Sourced from macos/assets so the wordmark is
# shared across platforms.
set(GOOGLE_LOGO "${CMAKE_SOURCE_DIR}/macos/assets/google_logo.png")
if(EXISTS "${GOOGLE_LOGO}")
    add_custom_command(TARGET earthview_handle_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOOGLE_LOGO}" "$<TARGET_FILE_DIR:earthview_handle_vk_win>/google_logo.png"
        COMMENT "Copying Google attribution logo next to exe"
    )
endif()

# Copy the DisplayXR shell sidecar (.displayxr.json + icons) next to the exe
# so the installer / shell scanner picks it up.
displayxr_install_manifest(earthview_handle_vk_win
    "${CMAKE_CURRENT_SOURCE_DIR}/displayxr")
