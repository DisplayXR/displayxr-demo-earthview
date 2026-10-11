// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// EarthView's adoption of the depth-aware cursor (XR_DXR_cursor_depth,
// runtime ADR-046 + Amendment 2). Same shape as displayxr-demo-modelviewer's
// common/model_cursor_depth.h (the pilot); docs/depth-cursor.md has the why.
//
// The runtime DRAWS the cursor: an arrow lifted just in front of whatever pops
// out of the glass under the pointer, nothing at all over content on or behind
// the glass (the OS cursor stays the cursor there). The app owns only the depth
// under the pointer, and displayxr-common's `dxr::CursorDepth` does the
// bookkeeping. This header is what every EarthView leg shares on top of it:
//
//   - the depth source: `TileRenderer::raycast` — the streamed tiles drawn
//     last frame (terrain AND buildings: Google Photorealistic 3D Tiles is one
//     mesh), per-tile BVH built at tile load. The tiles are the only geometry
//     in the scene: the sky is a clear colour at infinity and every UI surface
//     (HUD, attribution, reticle, header bar, buttons) is 2D, so the tiles ARE
//     "everything that sticks out".
//   - `DISPLAYXR_CURSOR_DEPTH=0` opts out (the extension is then not enabled
//     at all, so the runtime does no cursor work).
//   - `DISPLAYXR_CURSOR_DEPTH_UV=u,v` scripts the pointer (canvas UV, origin
//     top-left) for headless verification; the leg then ignores real motion.
//   - a throttled cost line: the ray cast's wall time per frame + the
//     load-thread BVH build totals.
//
// Per leg (macos/main.mm, linux/main.cpp, windows/main.cpp):
//
//     // instance: enable XR_DXR_cursor_depth if present && CursorDepthWanted()
//     earthview::CursorDepthAdoption cursorAdopt;
//     cursorAdopt.setup(cursor, g_tileRenderer, specVersionOr0);
//     cursor.setWindowCocoa(view);          // or setWindowWin32 / the manual path
//     // per frame, around xrLocateViews:
//     cursor.chain(locateInfo, viewState);  // ray cast vs LAST frame's views + tiles
//     xrLocateViews(...);
//     cursor.afterLocate(views, activeViewCount);
//     if (cursorAdopt.endFrame(cursor)) LOG_INFO("%s", cursorAdopt.summary());
//
// Space: the helper fires its rays from the located views, in the locate
// space. EarthView renders those views directly (rig pose identity; all
// navigation lives in the double world mapping xrFromEcef), and every draw
// matrix is xrFromEcef * yUpToZUp * Tile::getTransform() — so the ray, the
// views and the tiles share one space, and the ray cast reuses the draw
// matrices' inverses. `chain()` casts against the PREVIOUS frame's views; the
// draw list it reads is that same previous frame's (rebuilt after the locate),
// so views and world mapping always match.

#pragma once

#include "cursor_depth.h" // displayxr-common v2.29.0+
#include "cursor_depth_wanted.h"
#include "tile_renderer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace earthview {

class CursorDepthAdoption
{
public:
	//! enable(specVersion) (0 = extension not enabled: the helper stays inert
	//! and no tile BVH is ever built), the tile ray cast, and the scripted
	//! pointer. Call before the frame loop streams tiles. The window adapter
	//! stays with the leg (it is the only platform-specific part).
	void
	setup(dxr::CursorDepth &cursor, TileRenderer &renderer, uint32_t specVersion)
	{
		renderer_ = &renderer;
		renderer.setRaycastEnabled(specVersion > 0);
		cursor.enable(specVersion);
		const TileRenderer *r = &renderer;
		Stats *st = &stats_;
		cursor.setRayCast([r, st](const dxr::CursorRay &ray) -> std::optional<float> {
			const auto t0 = std::chrono::steady_clock::now();
			const float o[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
			const float d[3] = {ray.dir.x, ray.dir.y, ray.dir.z};
			float t = 0.0f;
			const bool hit = r->raycast(o, d, t);
			st->frameUs +=
			    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
			st->frameRays++;
			return hit ? std::optional<float>(t) : std::nullopt;
		});
		scripted_ = false;
		if (const char *uv = std::getenv("DISPLAYXR_CURSOR_DEPTH_UV")) {
			float u = 0.0f, v = 0.0f;
			if (std::sscanf(uv, "%f,%f", &u, &v) == 2) {
				cursor.setPointerUV(u, v);
				cursor.setWantPlacement(true); // the placement goes into the summary
				scripted_ = true;
			}
		}
	}

	//! The pointer is scripted (DISPLAYXR_CURSOR_DEPTH_UV): ignore real motion.
	bool
	scripted() const
	{
		return scripted_;
	}

	//! Call once per frame after afterLocate(). True when a cost summary is
	//! ready (every kSummaryFrames frames that ran the ray cast); read it with summary().
	bool
	endFrame(const dxr::CursorDepth &cursor)
	{
		if (stats_.frameRays > 0) {
			stats_.frames++;
			stats_.sumUs += stats_.frameUs;
			stats_.sumRays += stats_.frameRays;
			if (stats_.frameUs > stats_.maxUs) stats_.maxUs = stats_.frameUs;
			if (cursor.hint().hasContent) stats_.hitFrames++;
			if (cursor.drawState().willDraw) stats_.drawFrames++;
			if (renderer_) stats_.sumTiles += (uint64_t)renderer_->pickTileCount();
		}
		stats_.frameUs = 0.0;
		stats_.frameRays = 0;
		if (stats_.frames < kSummaryFrames) return false;
		std::snprintf(summary_, sizeof(summary_),
		              "cursor depth: ray cast %.1f us/frame mean, %.1f us max, %.1f rays/frame over %.0f "
		              "tiles, %u frames (hit %u, runtime drew %u)",
		              stats_.sumUs / stats_.frames, stats_.maxUs, (double)stats_.sumRays / stats_.frames,
		              (double)stats_.sumTiles / stats_.frames, stats_.frames, stats_.hitFrames,
		              stats_.drawFrames);
		if (renderer_) {
			const TileRenderer::BvhBuildStats b = renderer_->bvhBuildStats();
			const size_t n = std::strlen(summary_);
			std::snprintf(summary_ + n, sizeof(summary_) - n,
			              "; tile BVH build (load thread) %llu tiles / %.2f Mtri, %.2f ms mean, %.1f ms max, "
			              "%.0f MB resident",
			              (unsigned long long)b.tiles, (double)b.triangles * 1e-6,
			              b.tiles ? b.totalMs / (double)b.tiles : 0.0, b.maxMs, b.residentMB);
		}
		if (scripted_) {
			const XrCursorDepthPlacementDXR &pl = cursor.placement();
			const size_t n = std::strlen(summary_);
			std::snprintf(summary_ + n, sizeof(summary_) - n,
			              "; placement active=%d disparity=%.3f target=%.3f pos=(%.4f, %.4f, %.4f) h=%.4f",
			              (int)pl.isActive, pl.disparity, pl.targetDisparity, pl.pose.position.x,
			              pl.pose.position.y, pl.pose.position.z, pl.height);
		}
		stats_ = Stats{};
		return true;
	}
	const char *
	summary() const
	{
		return summary_;
	}

private:
	static constexpr uint32_t kSummaryFrames = 300;
	struct Stats
	{
		double frameUs = 0.0;
		uint32_t frameRays = 0;
		uint32_t frames = 0, hitFrames = 0, drawFrames = 0;
		uint64_t sumRays = 0, sumTiles = 0;
		double sumUs = 0.0, maxUs = 0.0;
	};
	const TileRenderer *renderer_ = nullptr;
	Stats stats_;
	bool scripted_ = false;
	char summary_[768] = {};
};

} // namespace earthview
