// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// The depth-aware cursor's opt-out, on its own so an xrCreateInstance site
// can test it without pulling in the tile renderer (and cesium-native's
// headers, which collide with <windows.h> macros such as OPAQUE).

#pragma once

#include <cstdlib>
#include <cstring>

namespace earthview {

//! False when DISPLAYXR_CURSOR_DEPTH=0: do not enable XR_DXR_cursor_depth.
inline bool
CursorDepthWanted()
{
	const char *e = std::getenv("DISPLAYXR_CURSOR_DEPTH");
	return !(e != nullptr && std::strcmp(e, "0") == 0);
}

} // namespace earthview
