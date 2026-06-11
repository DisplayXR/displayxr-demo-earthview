# EarthView M0 spike — headless Photorealistic-3D-Tiles → PNG proof.
# Validates: cesium-native build on macOS, API key + session chain, Draco
# decode, texture decode, tile selection. No window, no Vulkan (the Vulkan
# substrate is proven by displayxr-demo-modelviewer; M1 grafts onto it).
cmake_minimum_required(VERSION 3.25)
project(earthview_m0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CESIUM_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
# cesium-native pinned at v0.61.0 (clone per README; ezvcpkg bootstraps deps)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cesium-native
    cesium-native EXCLUDE_FROM_ALL)

add_executable(earthview_m0 main.cpp)
target_link_libraries(earthview_m0 PRIVATE
    Cesium3DTilesSelection
    CesiumCurl)
