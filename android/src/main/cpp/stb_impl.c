/* Copyright 2026, Leia Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Single stb_image_write implementation TU for the Android leg. tile_renderer.cpp
 * forward-declares `extern "C" int stbi_write_png(...)` and expects the symbol to
 * be linked from displayxr-common (which the desktop legs link but the Android
 * leg does not). Compiled as C so stbi_write_png has C linkage, matching that
 * extern "C" declaration. Header comes from the repo's spike/ (added to the
 * include path in CMakeLists.txt).
 */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
