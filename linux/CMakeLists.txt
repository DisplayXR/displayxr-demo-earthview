# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Standalone build — lives inside displayxr-demo-earthview repo as
# linux/CMakeLists.txt. Assumes sibling dirs: ../common, ../tiles_common,
# ../openxr_includes. Top-level CMakeLists.txt is expected to have already
# added ../common and ../tiles_common as subdirectories (and cesium-native),
# exposing the `sr_common_base` and `tile_renderer` targets.
#
# linux/main.cpp is the Linux entry point (issue #19, M8 Linux epic). It is a
# HOSTED-NULL, reduced harness: Vulkan (system libvulkan) + OpenXR session +
# cesium tile streaming, with NO app-provided window (the runtime self-creates
# one) and no HUD/input. BUILD-GREEN scope — see docs/roadmap.md (M8) and the
# runtime repo's docs/guides/linux-demo-port.md. The faithful window arm
# (XR_DXR_xlib_window_binding) is Phase-3 hardware-gated.

cmake_minimum_required(VERSION 3.25)

if(APPLE OR WIN32)
    return()
endif()

find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR loader — prefer an installed/dev loader (build_linux.sh installs one
# under /tmp/openxr-install and points CMAKE_PREFIX_PATH at it), else build the
# pinned Khronos loader from source. Pin kept at release-1.1.43 to match the
# loader build_linux.sh provisions and the other demos (do NOT bump; the
# runtime-wide loader pin is 1.1.43 while the vendored openxr_includes headers
# are newer — that drift is expected, see the linux-demo-port playbook).
set(OpenXR_FOUND FALSE)
find_package(OpenXR CONFIG QUIET)
if(OpenXR_FOUND)
    message(STATUS "OpenXR loader: found via find_package (OpenXR::openxr_loader)")
    set(EV_OPENXR_LOADER OpenXR::openxr_loader)
else()
    message(STATUS "OpenXR loader: not installed — fetching Khronos OpenXR-SDK release-1.1.43")
    include(FetchContent)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(BUILD_WITH_SYSTEM_JSONCPP ON CACHE BOOL "" FORCE)
    set(CMAKE_MAP_IMPORTED_CONFIG_RELEASE ";RELEASE;NOCONFIG")
    FetchContent_Declare(OpenXR
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.1.43
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(OpenXR)
    set(EV_OPENXR_LOADER openxr_loader)
endif()

add_executable(earthview_handle_vk_linux
    main.cpp
    # stbi_write_png impl: displayxr-common ships it only on WIN32/APPLE, but
    # tiles_common's TileRenderer::dumpColorTarget references it. See the TU.
    stb_image_write_impl.cpp
)

target_include_directories(earthview_handle_vk_linux PRIVATE
    ${CMAKE_SOURCE_DIR}/openxr_includes
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/tiles_common
)

target_link_libraries(earthview_handle_vk_linux PRIVATE
    sr_common_base
    tile_renderer
    ${EV_OPENXR_LOADER}
    Vulkan::Vulkan
)

# Copy the OpenXR loader .so next to the binary when it's an imported target,
# so the app runs without extra LD_LIBRARY_PATH setup (dev convenience; not
# exercised on CI, which never runs the binary).
if(TARGET OpenXR::openxr_loader)
    get_target_property(_oxr_loc OpenXR::openxr_loader IMPORTED_LOCATION)
    if(NOT _oxr_loc)
        get_target_property(_oxr_loc OpenXR::openxr_loader IMPORTED_LOCATION_RELEASE)
    endif()
    if(_oxr_loc)
        add_custom_command(TARGET earthview_handle_vk_linux POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_oxr_loc}" "$<TARGET_FILE_DIR:earthview_handle_vk_linux>"
            COMMENT "Copying OpenXR loader next to earthview_handle_vk_linux")
    endif()
endif()

# Copy the Google attribution logo next to the exe (HUD strip asset — Map
# Tiles API policy). Sourced from macos/assets so the wordmark is shared.
set(GOOGLE_LOGO "${CMAKE_SOURCE_DIR}/macos/assets/google_logo.png")
if(EXISTS "${GOOGLE_LOGO}")
    add_custom_command(TARGET earthview_handle_vk_linux POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOOGLE_LOGO}"
            "$<TARGET_FILE_DIR:earthview_handle_vk_linux>/google_logo.png"
        COMMENT "Copying Google attribution logo next to exe")
endif()
