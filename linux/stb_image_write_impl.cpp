// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
//
// stb_image_write implementation TU for the Linux target.
//
// tiles_common's TileRenderer::dumpColorTarget() (dev-only self-capture)
// references stbi_write_png via a file-scope `extern "C"` forward declaration,
// expecting the implementation to be linked in from displayxr::common. But
// displayxr-common supplies STB_IMAGE_WRITE_IMPLEMENTATION only on WIN32
// (d3d11_renderer.cpp) and APPLE (atlas_capture_macos.mm) — its Linux neutral
// subset defines NO stb impl. So the Linux demo target owns exactly one
// write-impl TU here. stb_image_write.h resolves via displayxr::common's
// propagated PUBLIC include dir (its vendored common/ directory).
//
// Exactly one TU per link target may define STB_IMAGE_WRITE_IMPLEMENTATION;
// this is the only one on Linux (the WIN32/APPLE definers are gated out).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
