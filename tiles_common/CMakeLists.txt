# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: Apache-2.0
#
# tiles_common — the EarthView scene layer: cesium-native glue (tile_engine),
# the Vulkan tile renderer (IPrepareRendererResources impl + draw), and the
# double-precision geo/RTC math. Mirrors the role model_common/ plays in
# displayxr-demo-modelviewer, with cesium-native as the sole scene source
# (no model loaders, no PBR/IBL — photogrammetry is unlit textured).

cmake_minimum_required(VERSION 3.25)
project(tile_renderer_lib LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)

# ── SPIR-V shader compilation (carried over from modelviewer) ─────────────
find_program(GLSLANGVALIDATOR_COMMAND glslangValidator)
if(NOT GLSLANGVALIDATOR_COMMAND)
    message(FATAL_ERROR "glslangValidator required — install the Vulkan SDK")
endif()

set(SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_SOURCES
    shaders/tile.vert
    shaders/tile.frag
)

set(SHADER_HEADERS "")
foreach(GLSL_REL ${SHADER_SOURCES})
    get_filename_component(GLSL_NAME ${GLSL_REL} NAME)
    string(MAKE_C_IDENTIFIER ${GLSL_NAME} IDENTIFIER)
    set(VARNAME "${IDENTIFIER}_data")
    set(HEADER "${CMAKE_CURRENT_BINARY_DIR}/${GLSL_NAME}.h")
    set(GLSL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/${GLSL_REL}")
    add_custom_command(
        OUTPUT ${HEADER}
        COMMAND ${GLSLANGVALIDATOR_COMMAND}
            -V --target-env vulkan1.2
            -I${SHADER_INCLUDE_DIR}
            ${GLSL_SRC}
            --vn ${VARNAME}
            -o ${HEADER}
        DEPENDS ${GLSL_SRC}
        COMMENT "Compiling SPIR-V: ${GLSL_NAME}"
    )
    list(APPEND SHADER_HEADERS ${HEADER})
endforeach()

add_library(tile_renderer STATIC
    geo_math.h
    geo_math.cpp
    tile_engine.h
    tile_engine.cpp
    tile_renderer.h
    tile_renderer.cpp
    model_vulkan_utils.h
    model_vulkan_utils.cpp
    ${SHADER_HEADERS}
)

target_include_directories(tile_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(tile_renderer PUBLIC
    Vulkan::Vulkan
    Cesium3DTilesSelection
    Cesium3DTilesContent
    CesiumCurl
)
