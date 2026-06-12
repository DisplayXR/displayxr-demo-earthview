// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// tile_engine — Cesium3DTilesSelection::Tileset wrapper for Google
// Photorealistic 3D Tiles (ported from the M0 spike, spike/main.cpp).
// Owns the accessor / task processor / credit system / tileset; the caller
// owns the IPrepareRendererResources (TileRenderer) and the per-frame camera.
//
// Threading: update() must be called from ONE thread (the render thread) —
// that thread becomes cesium's "main thread" (prepareInMainThread / free are
// dispatched inside updateView).

#pragma once

#include <Cesium3DTilesSelection/ViewUpdateResult.h>

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Cesium3DTilesSelection {
class Tileset;
class IPrepareRendererResources;
} // namespace Cesium3DTilesSelection

namespace geo {
struct GeoCamera;
}

struct AttributionInfo
{
	// Frequency-sorted copyright strings (most-attributed first), HTML
	// stripped. Policy: display all of them + the Google logo (PRD §7.3).
	std::vector<std::string> credits;
	int tilesInFlight = 0;
	int renderTiles = 0;
	double residentMB = 0.0; // engine-side estimate (GPU bytes live in TileRenderer)
};

class TileEngine
{
public:
	TileEngine();
	~TileEngine();

	// Key lookup (GOOGLE_MAPS_API_KEY env, else earthview.ini next to the
	// exe / repo root) + tileset creation. Returns false when keyless — the
	// app stays up and shows the how-to-get-a-key card (PRD §7.4).
	// `prepare` is non-owning; it must outlive this TileEngine.
	bool
	init(Cesium3DTilesSelection::IPrepareRendererResources *prepare);

	// Destroy the Tileset (free()s every live tile through the renderer, on
	// the calling thread). MUST run before TileRenderer::cleanup() and before
	// the VkDevice dies.
	void
	shutdown();

	bool
	hasKey() const
	{
		return !key_.empty();
	}

	// Once per frame from the render thread. One ViewState from the
	// center-eye camera (PRD §6.2): viewport = ONE view tile's pixels,
	// fovs = union across views. Both eyes draw the same selected set.
	const Cesium3DTilesSelection::ViewUpdateResult &
	update(const geo::GeoCamera &cam, double viewW, double viewH, double hfovRad, double vfovRad);

	// True once the selection carries actual render content (M0 fact 4:
	// empty queues alone fire before any mesh loads).
	bool
	hasRenderableContent() const
	{
		return lastRenderTiles_ > 0;
	}

	// Attribution feed, recomputed at most every `minIntervalFrames` calls
	// (HUD refreshes ~2 Hz; counting credits every frame is wasted work).
	const AttributionInfo &
	attribution();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	std::string key_;
	int lastRenderTiles_ = 0;
	int frameCounter_ = 0;
	AttributionInfo attribution_;
};

// Key lookup helper (shared with the first-run HUD card).
std::string
earthviewGetApiKey();
