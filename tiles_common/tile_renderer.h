// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// tile_renderer — Vulkan renderer for cesium-native tiles: implements
// IPrepareRendererResources (tile glTF -> GPU buffers/textures) and draws
// the per-frame selected set, unlit textured (photogrammetry has baked
// lighting), into a per-eye viewport of the app swapchain.
//
// Threading contract (verified against cesium-native v0.61 headers):
//  - prepareInLoadThread: WORKER thread. CPU-only — packs vertices/indices/
//    RGBA8 texels into a heap TileStaging. NO Vulkan calls.
//  - prepareInMainThread + free: ALWAYS the thread that calls
//    Tileset::updateView (our render thread). All Vulkan object create /
//    destroy happens there — no locks needed.
//  - free() destroys immediately: the frame loop is fully serialized
//    (renderEye ends in vkQueueWaitIdle, and updateView runs before the
//    frame's draws), so no tile resource is ever GPU-in-flight when freed.
//
// Lifetime: the app owns one TileRenderer; the TileEngine (Tileset) must be
// destroyed FIRST (its destructor free()s every live tile through this
// object), then TileRenderer::cleanup().

#pragma once

#include "model_vulkan_utils.h"

#include <Cesium3DTilesSelection/IPrepareRendererResources.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace Cesium3DTilesSelection {
class ViewUpdateResult;
}

struct TileGpu; // opaque GPU residence record (defined in tile_renderer.cpp)

class TileRenderer : public Cesium3DTilesSelection::IPrepareRendererResources
{
public:
	bool
	init(VkInstance instance,
	     VkPhysicalDevice physicalDevice,
	     VkDevice device,
	     VkQueue queue,
	     uint32_t queueFamilyIndex,
	     uint32_t renderWidth,
	     uint32_t renderHeight);
	void
	cleanup();
	~TileRenderer() override;

	// ── IPrepareRendererResources ────────────────────────────────────────
	CesiumAsync::Future<Cesium3DTilesSelection::TileLoadResultAndRenderResources>
	prepareInLoadThread(const CesiumAsync::AsyncSystem &asyncSystem,
	                    Cesium3DTilesSelection::TileLoadResult &&tileLoadResult,
	                    const glm::dmat4 &transform,
	                    const std::any &rendererOptions) override;

	void *
	prepareInMainThread(Cesium3DTilesSelection::Tile &tile, void *pLoadThreadResult) override;

	void
	free(Cesium3DTilesSelection::Tile &tile,
	     void *pLoadThreadResult,
	     void *pMainThreadResult) noexcept override;

	// Raster overlays unused for Google P3DT — stub no-ops.
	void *
	prepareRasterInLoadThread(CesiumGltf::ImageAsset &image,
	                          const std::any &rendererOptions) override
	{
		return nullptr;
	}
	void *
	prepareRasterInMainThread(CesiumRasterOverlays::RasterOverlayTile &rasterTile,
	                          void *pLoadThreadResult) override
	{
		return nullptr;
	}
	void
	freeRaster(const CesiumRasterOverlays::RasterOverlayTile &rasterTile,
	           void *pLoadThreadResult,
	           void *pMainThreadResult) noexcept override
	{
	}
	void
	attachRasterInMainThread(const Cesium3DTilesSelection::Tile &tile,
	                         int32_t overlayTextureCoordinateID,
	                         const CesiumRasterOverlays::RasterOverlayTile &rasterTile,
	                         void *pMainThreadRendererResources,
	                         const glm::dvec2 &translation,
	                         const glm::dvec2 &scale) override
	{
	}
	void
	detachRasterInMainThread(const Cesium3DTilesSelection::Tile &tile,
	                         int32_t overlayTextureCoordinateID,
	                         const CesiumRasterOverlays::RasterOverlayTile &rasterTile,
	                         void *pMainThreadRendererResources) noexcept override
	{
	}

	// ── Per-frame draw ───────────────────────────────────────────────────

	struct DrawItem
	{
		const TileGpu *gpu;
		float model[16]; // XR-space model matrix (double product, cast once)
	};

	// Build the frame's draw list from the selection. xrFromEcef is the
	// double world mapping from geo_math (camera-centric or diorama); the
	// Y-up→Z-up premultiply onto Tile::getTransform() happens here (M0
	// fact 2). Call once per frame; both eyes share the result.
	std::vector<DrawItem>
	buildDrawList(const Cesium3DTilesSelection::ViewUpdateResult &result,
	              const glm::dmat4 &xrFromEcef) const;

	// Mirror of ModelRenderer::renderEye: render the draw list into an
	// internal color+depth target, then blit into the [vpX,vpY,vpW,vpH]
	// region of the swapchain image. Clears to sky. Submit + wait (M1;
	// batching is an M1.x optimization).
	void
	renderEye(VkImage swapchainImage,
	          VkFormat swapchainFormat,
	          uint32_t imageWidth,
	          uint32_t imageHeight,
	          uint32_t viewportX,
	          uint32_t viewportY,
	          uint32_t viewportWidth,
	          uint32_t viewportHeight,
	          const float viewMatrix[16],
	          const float projMatrix[16],
	          const std::vector<DrawItem> &drawList);

	// Read one depth texel ([0,1], 1.0 = far/no hit) from the internal depth
	// image at framebuffer pixel (px, py) — the LAST renderEye's contents.
	// Used by double-click picking (depth-readback unproject). Submit+wait.
	float
	readDepth(uint32_t px, uint32_t py);

	// Dump the internal color target (the LAST renderEye's mono content, the
	// top-left w×h region) to a PNG. Self-verification on vk_native, where the
	// runtime atlas capture is unreliable. Submit+wait; dev/diagnostic only.
	void
	dumpColorTarget(const char *path, uint32_t w, uint32_t h);

	// Supersample factor (1 = off): renderEye draws each view at N× the final
	// per-view resolution and downsamples on the blit (geometry-edge AA). Costs
	// ~N² fragment work. Clamped to the internal target. Live-tweakable (the
	// shell cycles it with the 'S' key for A/B).
	void
	setSupersample(uint32_t n)
	{
		ssaaFactor_ = (n < 1) ? 1u : n;
	}
	uint32_t
	supersample() const
	{
		return ssaaFactor_;
	}

	// HUD stats.
	double
	gpuResidentMB() const
	{
		return (double)gpuBytes_ / (1024.0 * 1024.0);
	}
	int
	liveTileCount() const
	{
		return (int)liveTiles_.size();
	}
	// Selected-but-not-yet-GPU-prepared tiles skipped by the last
	// buildDrawList (diagnostic: persistent >0 = upload starvation = holes).
	int
	lastStagingSkipped() const
	{
		return lastStagingSkipped_;
	}

private:
	bool
	createRenderTargets();
	bool
	ensureTargets(uint32_t w, uint32_t h);
	bool
	createPipeline();
	bool
	createSamplerAndDefaults();
	ModelImage
	uploadTexture(const uint8_t *rgba, uint32_t w, uint32_t h);
	VkDescriptorSet
	allocateTextureSet(VkImageView view);

	VkInstance instance_ = VK_NULL_HANDLE;
	VkPhysicalDevice physDevice_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	VkQueue queue_ = VK_NULL_HANDLE;
	uint32_t queueFamily_ = 0;
	uint32_t width_ = 0, height_ = 0;
	// Supersample anti-aliasing: renderEye draws each view at kSsaa× the final
	// per-view resolution into the (swapchain-sized) internal target, then the
	// blit downsamples to the swapchain tile (geometry-edge AA). lastSsScale*_
	// carry the actual src/dst ratio (clamped to the target) so readDepth and
	// dumpColorTarget sample the supersampled buffer at the right pixel.
	float lastSsScaleX_ = 1.0f, lastSsScaleY_ = 1.0f;
	uint32_t ssaaFactor_ = 1;  // 1 = off (default); shell cycles 1→2→4 via 'S'
	bool initialized_ = false;

	VkCommandPool cmdPool_ = VK_NULL_HANDLE;
	VkRenderPass renderPass_ = VK_NULL_HANDLE;
	VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
	ModelImage colorImage_;
	ModelImage depthImage_;
	// SRGB internal target: shader writes linear, attachment encodes, blit to
	// the sRGB swapchain is value-preserving (INV-4.6).
	VkFormat colorFormat_ = VK_FORMAT_R8G8B8A8_SRGB;
	VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;

	VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
	VkPipeline pipeline_ = VK_NULL_HANDLE;
	VkDescriptorPool descPool_ = VK_NULL_HANDLE;
	VkSampler sampler_ = VK_NULL_HANDLE;
	ModelImage whiteTex_;
	VkDescriptorSet whiteSet_ = VK_NULL_HANDLE;

	std::unordered_set<TileGpu *> liveTiles_;
	uint64_t gpuBytes_ = 0;
	mutable int lastStagingSkipped_ = 0;
};
