// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// See tile_renderer.h for the threading contract. The Vulkan plumbing
// (internal target + viewport blit, negative-height viewport, texture upload
// with mip chain) mirrors modelviewer's ModelRenderer — proven on this exact
// MoltenVK stack.

#include "tile_renderer.h"
#include "geo_math.h"

#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/ViewUpdateResult.h>
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumGltf/AccessorView.h>
#include <CesiumGltf/Model.h>

#include <glm/gtc/type_ptr.hpp>

// stb_image_write implementation is already linked from displayxr-common
// (atlas_capture); forward-declare the one entry point we use.
extern "C" int stbi_write_png(const char *filename, int w, int h, int comp, const void *data,
                              int stride_bytes);

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Generated SPIR-V headers (tiles_common/CMakeLists.txt glslang block).
#include "tile.vert.h"
#include "tile.frag.h"

namespace {

constexpr float kSkyColor[4] = {0.53f, 0.75f, 0.92f, 1.0f};

struct TileVertex
{
	float pos[3];
	float uv[2];
};

struct StagedPrimitive
{
	uint32_t firstIndex = 0;
	uint32_t indexCount = 0;
	int32_t vertexOffset = 0;
	int textureIndex = -1; // into TileStaging::textures; -1 = white + gray tint
	float tint[3] = {1.0f, 1.0f, 1.0f};
};

struct StagedTexture
{
	uint32_t width = 0, height = 0;
	std::vector<uint8_t> rgba; // tightly packed RGBA8
};

// prepareInLoadThread output: pure CPU data, deleted by prepareInMainThread
// (or by free() if the tile dies before reaching the main thread).
struct TileStaging
{
	std::vector<TileVertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<StagedTexture> textures;
	std::vector<StagedPrimitive> prims;
};

struct PushBlock
{
	float mvp[16];
	float tint[4];
};

} // namespace

// A tile's getRenderResources() can still hold the LOAD-thread result
// (TileStaging*) when the tile is selected before its main-thread prepare slice
// runs (mainThreadLoadingTimeLimit batching). The magic tag lets buildDrawList
// tell a real TileGpu from a not-yet-prepared staging pointer.
constexpr uint64_t kTileGpuMagic = 0x4556544750555F4DULL; // "EVTGPU_M"

// prepareInMainThread output: GPU residence for one tile.
struct TileGpu
{
	uint64_t magic = kTileGpuMagic;
	ModelBuffer vbuf;
	ModelBuffer ibuf;
	std::vector<ModelImage> textures;
	std::vector<VkDescriptorSet> texSets; // parallel to textures
	std::vector<StagedPrimitive> prims;
	uint64_t bytes = 0;
};

// ── Load thread: glTF -> TileStaging (CPU only) ──────────────────────────

namespace {

// Expand an ImageAsset (1/3/4 channels, byte) into tight RGBA8.
bool
stageTexture(const CesiumGltf::ImageAsset &img, StagedTexture &out)
{
	if (img.pixelData.empty() || img.bytesPerChannel != 1 || img.width <= 0 || img.height <= 0) {
		return false;
	}
	const int ch = img.channels;
	if (ch != 1 && ch != 3 && ch != 4) {
		return false;
	}
	out.width = (uint32_t)img.width;
	out.height = (uint32_t)img.height;
	const size_t n = (size_t)img.width * (size_t)img.height;
	out.rgba.resize(n * 4);
	const uint8_t *src = reinterpret_cast<const uint8_t *>(img.pixelData.data());
	for (size_t i = 0; i < n; ++i) {
		uint8_t r = src[i * ch];
		uint8_t g = ch >= 3 ? src[i * ch + 1] : r;
		uint8_t b = ch >= 3 ? src[i * ch + 2] : r;
		uint8_t a = ch == 4 ? src[i * ch + 3] : 255;
		out.rgba[i * 4 + 0] = r;
		out.rgba[i * 4 + 1] = g;
		out.rgba[i * 4 + 2] = b;
		out.rgba[i * 4 + 3] = a;
	}
	return true;
}

void
stagePrimitive(const CesiumGltf::Model &model,
               const CesiumGltf::MeshPrimitive &prim,
               const glm::dmat4 &nodeTransform,
               TileStaging &staging,
               std::vector<int> &imageToStaged)
{
	using namespace CesiumGltf;

	if (prim.mode != MeshPrimitive::Mode::TRIANGLES) {
		return;
	}
	auto posIt = prim.attributes.find("POSITION");
	if (posIt == prim.attributes.end()) {
		return;
	}
	AccessorView<glm::vec3> positions(model, posIt->second);
	if (positions.status() != AccessorViewStatus::Valid) {
		return;
	}

	AccessorView<glm::vec2> uvs;
	if (auto uvIt = prim.attributes.find("TEXCOORD_0"); uvIt != prim.attributes.end()) {
		uvs = AccessorView<glm::vec2>(model, uvIt->second);
	}
	const bool hasUVs = uvs.status() == AccessorViewStatus::Valid;

	StagedPrimitive sp;
	sp.vertexOffset = (int32_t)staging.vertices.size();
	sp.firstIndex = (uint32_t)staging.indices.size();

	// Base color texture + factor (same walk as the spike's rasterizer).
	if (prim.material >= 0 && prim.material < (int32_t)model.materials.size()) {
		const Material &mat = model.materials[(size_t)prim.material];
		if (mat.pbrMetallicRoughness) {
			const auto &f = mat.pbrMetallicRoughness->baseColorFactor;
			if (f.size() >= 3) {
				sp.tint[0] = (float)f[0];
				sp.tint[1] = (float)f[1];
				sp.tint[2] = (float)f[2];
			}
			if (mat.pbrMetallicRoughness->baseColorTexture) {
				int32_t texIdx = mat.pbrMetallicRoughness->baseColorTexture->index;
				if (texIdx >= 0 && texIdx < (int32_t)model.textures.size()) {
					int32_t src = model.textures[(size_t)texIdx].source;
					if (src >= 0 && src < (int32_t)model.images.size() &&
					    model.images[(size_t)src].pAsset) {
						if (imageToStaged[(size_t)src] < 0) {
							StagedTexture st;
							if (stageTexture(*model.images[(size_t)src].pAsset, st)) {
								imageToStaged[(size_t)src] =
								    (int)staging.textures.size();
								staging.textures.push_back(std::move(st));
							}
						}
						sp.textureIndex = imageToStaged[(size_t)src];
					}
				}
			}
		}
	}
	if (sp.textureIndex < 0) {
		// Textureless photogrammetry chunk: flat gray via white tex × tint.
		sp.tint[0] *= 0.67f;
		sp.tint[1] *= 0.67f;
		sp.tint[2] *= 0.67f;
	}

	// Vertices: bake the (tile-local, small) node transform in double, store
	// float. Tile-scale magnitudes keep float32 exact enough (M0 fact 5 —
	// the big translations live in the per-frame double model matrix).
	for (int64_t i = 0; i < positions.size(); ++i) {
		glm::vec3 p = positions[i];
		glm::dvec4 lp = nodeTransform * glm::dvec4(p.x, p.y, p.z, 1.0);
		TileVertex v;
		v.pos[0] = (float)lp.x;
		v.pos[1] = (float)lp.y;
		v.pos[2] = (float)lp.z;
		if (hasUVs && i < uvs.size()) {
			glm::vec2 uv = uvs[i];
			v.uv[0] = uv.x;
			v.uv[1] = uv.y;
		} else {
			v.uv[0] = v.uv[1] = 0.0f;
		}
		staging.vertices.push_back(v);
	}

	// Indices: u8/u16/u32 -> u32 (three-way AccessorView fetch, as in the
	// spike); -1 accessor = non-indexed.
	if (prim.indices >= 0) {
		AccessorView<uint16_t> idx16(model, prim.indices);
		AccessorView<uint32_t> idx32(model, prim.indices);
		AccessorView<uint8_t> idx8(model, prim.indices);
		if (idx16.status() == AccessorViewStatus::Valid) {
			for (int64_t i = 0; i < idx16.size(); ++i) {
				staging.indices.push_back(idx16[i]);
			}
		} else if (idx32.status() == AccessorViewStatus::Valid) {
			for (int64_t i = 0; i < idx32.size(); ++i) {
				staging.indices.push_back(idx32[i]);
			}
		} else if (idx8.status() == AccessorViewStatus::Valid) {
			for (int64_t i = 0; i < idx8.size(); ++i) {
				staging.indices.push_back(idx8[i]);
			}
		} else {
			return; // unsupported index type — drop primitive
		}
	} else {
		for (int64_t i = 0; i < positions.size(); ++i) {
			staging.indices.push_back((uint32_t)i);
		}
	}
	sp.indexCount = (uint32_t)staging.indices.size() - sp.firstIndex;
	if (sp.indexCount > 0) {
		staging.prims.push_back(sp);
	}
}

} // namespace

CesiumAsync::Future<Cesium3DTilesSelection::TileLoadResultAndRenderResources>
TileRenderer::prepareInLoadThread(const CesiumAsync::AsyncSystem &asyncSystem,
                                  Cesium3DTilesSelection::TileLoadResult &&tileLoadResult,
                                  const glm::dmat4 &transform,
                                  const std::any &rendererOptions)
{
	const CesiumGltf::Model *pModel =
	    std::get_if<CesiumGltf::Model>(&tileLoadResult.contentKind);
	if (!pModel) {
		// External-tileset / empty tiles (M0 fact 4) — no render resources.
		return asyncSystem.createResolvedFuture(
		    Cesium3DTilesSelection::TileLoadResultAndRenderResources{
		        std::move(tileLoadResult), nullptr});
	}

	auto *staging = new TileStaging();
	std::vector<int> imageToStaged(pModel->images.size(), -1);
	pModel->forEachPrimitiveInScene(
	    -1, [&](const CesiumGltf::Model &m, const CesiumGltf::Node &, const CesiumGltf::Mesh &,
	            const CesiumGltf::MeshPrimitive &prim, const glm::dmat4 &nodeTransform) {
		    stagePrimitive(m, prim, nodeTransform, *staging, imageToStaged);
	    });

	if (staging->prims.empty()) {
		delete staging;
		staging = nullptr;
	}
	return asyncSystem.createResolvedFuture(
	    Cesium3DTilesSelection::TileLoadResultAndRenderResources{std::move(tileLoadResult),
	                                                             staging});
}

// ── Main (render) thread: TileStaging -> TileGpu ─────────────────────────

void *
TileRenderer::prepareInMainThread(Cesium3DTilesSelection::Tile &tile, void *pLoadThreadResult)
{
	auto *staging = static_cast<TileStaging *>(pLoadThreadResult);
	if (!staging || !initialized_) {
		delete staging;
		return nullptr;
	}

	auto *gpu = new TileGpu();
	gpu->prims = staging->prims;

	const VkDeviceSize vbytes = staging->vertices.size() * sizeof(TileVertex);
	const VkDeviceSize ibytes = staging->indices.size() * sizeof(uint32_t);
	gpu->vbuf = modelCreateBuffer(device_, physDevice_, vbytes,
	                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
	                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	gpu->ibuf = modelCreateBuffer(device_, physDevice_, ibytes,
	                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
	                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	bool ok = gpu->vbuf.buffer != VK_NULL_HANDLE && gpu->ibuf.buffer != VK_NULL_HANDLE;
	ok = ok && modelUploadBuffer(device_, physDevice_, queue_, cmdPool_, gpu->vbuf,
	                             staging->vertices.data(), vbytes);
	ok = ok && modelUploadBuffer(device_, physDevice_, queue_, cmdPool_, gpu->ibuf,
	                             staging->indices.data(), ibytes);
	gpu->bytes = vbytes + ibytes;

	if (ok) {
		gpu->textures.reserve(staging->textures.size());
		gpu->texSets.reserve(staging->textures.size());
		for (const auto &st : staging->textures) {
			ModelImage img = uploadTexture(st.rgba.data(), st.width, st.height);
			VkDescriptorSet set =
			    img.view != VK_NULL_HANDLE ? allocateTextureSet(img.view) : VK_NULL_HANDLE;
			gpu->bytes += (uint64_t)st.width * st.height * 4 * 4 / 3; // + mips
			gpu->textures.push_back(img);
			gpu->texSets.push_back(set);
		}
	}

	delete staging;
	if (!ok) {
		std::fprintf(stderr, "TileRenderer: GPU upload failed for a tile (dropped)\n");
		free(tile, nullptr, gpu);
		return nullptr;
	}
	liveTiles_.insert(gpu);
	gpuBytes_ += gpu->bytes;
	return gpu;
}

void
TileRenderer::free(Cesium3DTilesSelection::Tile &tile,
                   void *pLoadThreadResult,
                   void *pMainThreadResult) noexcept
{
	delete static_cast<TileStaging *>(pLoadThreadResult);

	auto *gpu = static_cast<TileGpu *>(pMainThreadResult);
	if (!gpu) {
		return;
	}
	// Render-thread call (contract) + the frame is queue-wait-idle serialized
	// (renderEye submits and waits) → nothing here is GPU-in-flight.
	if (liveTiles_.erase(gpu)) {
		gpuBytes_ -= gpu->bytes;
	}
	if (device_ != VK_NULL_HANDLE) {
		for (VkDescriptorSet set : gpu->texSets) {
			if (set != VK_NULL_HANDLE) {
				vkFreeDescriptorSets(device_, descPool_, 1, &set);
			}
		}
		for (auto &img : gpu->textures) {
			if (img.image != VK_NULL_HANDLE) {
				modelDestroyImage(device_, img);
			}
		}
		if (gpu->vbuf.buffer != VK_NULL_HANDLE) {
			modelDestroyBuffer(device_, gpu->vbuf);
		}
		if (gpu->ibuf.buffer != VK_NULL_HANDLE) {
			modelDestroyBuffer(device_, gpu->ibuf);
		}
	}
	gpu->magic = 0; // poison: a stale draw-list pointer now fails the tag check
	delete gpu;
}

// ── Draw-list build (doubles in, floats out) ─────────────────────────────

std::vector<TileRenderer::DrawItem>
TileRenderer::buildDrawList(const Cesium3DTilesSelection::ViewUpdateResult &result,
                            const glm::dmat4 &xrFromEcef) const
{
	std::vector<DrawItem> items;
	items.reserve(result.tilesToRenderThisFrame.size());
	lastStagingSkipped_ = 0;
	const glm::dmat4 yz = geo::yUpToZUp();
	for (const auto &pTile : result.tilesToRenderThisFrame) {
		const auto &content = pTile->getContent();
		if (!content.isRenderContent()) {
			continue;
		}
		auto *gpu = static_cast<TileGpu *>(content.getRenderContent()->getRenderResources());
		if (!gpu || gpu->magic != kTileGpuMagic) {
			lastStagingSkipped_++;
			continue; // null, or load-thread staging not yet GPU-prepared
		}
		// Full double product — the giant ECEF translations cancel here, then
		// one cast to float (M0 fact 5 precision scheme).
		glm::dmat4 m = xrFromEcef * yz * pTile->getTransform();
		DrawItem item;
		item.gpu = gpu;
		glm::mat4 mf(m);
		std::memcpy(item.model, glm::value_ptr(mf), sizeof(item.model));
		items.push_back(item);
	}
	return items;
}

// ── Vulkan setup (mirrors ModelRenderer; see header) ─────────────────────

bool
TileRenderer::init(VkInstance instance,
                   VkPhysicalDevice physicalDevice,
                   VkDevice device,
                   VkQueue queue,
                   uint32_t queueFamilyIndex,
                   uint32_t renderWidth,
                   uint32_t renderHeight)
{
	instance_ = instance;
	physDevice_ = physicalDevice;
	device_ = device;
	queue_ = queue;
	queueFamily_ = queueFamilyIndex;
	width_ = renderWidth;
	height_ = renderHeight;

	VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = queueFamilyIndex;
	if (vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool_) != VK_SUCCESS) {
		std::fprintf(stderr, "TileRenderer: failed to create command pool\n");
		return false;
	}

	if (!createRenderTargets() || !createPipeline() || !createSamplerAndDefaults()) {
		return false;
	}
	initialized_ = true;
	std::printf("TileRenderer: initialized (%ux%u)\n", width_, height_);
	return true;
}

bool
TileRenderer::createRenderTargets()
{
	VkAttachmentDescription atts[2] = {};
	atts[0].format = colorFormat_;
	atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
	atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	atts[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	atts[1].format = depthFormat_;
	atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
	atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &colorRef;
	sub.pDepthStencilAttachment = &depthRef;

	VkSubpassDependency deps[2] = {};
	deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass = 0;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	deps[0].srcAccessMask = 0;
	deps[0].dstAccessMask =
	    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	deps[1].srcSubpass = 0;
	deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rpci.attachmentCount = 2;
	rpci.pAttachments = atts;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &sub;
	rpci.dependencyCount = 2;
	rpci.pDependencies = deps;
	if (vkCreateRenderPass(device_, &rpci, nullptr, &renderPass_) != VK_SUCCESS) {
		return false;
	}
	return ensureTargets(width_, height_);
}

bool
TileRenderer::ensureTargets(uint32_t w, uint32_t h)
{
	if (w == 0) {
		w = 1;
	}
	if (h == 0) {
		h = 1;
	}
	if (device_ != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(device_);
	}
	if (framebuffer_ != VK_NULL_HANDLE) {
		vkDestroyFramebuffer(device_, framebuffer_, nullptr);
		framebuffer_ = VK_NULL_HANDLE;
	}
	if (colorImage_.image != VK_NULL_HANDLE) {
		modelDestroyImage(device_, colorImage_);
	}
	if (depthImage_.image != VK_NULL_HANDLE) {
		modelDestroyImage(device_, depthImage_);
	}
	width_ = w;
	height_ = h;

	colorImage_ = modelCreateImage2D(device_, physDevice_, w, h, colorFormat_,
	                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	if (colorImage_.image == VK_NULL_HANDLE) {
		return false;
	}

	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = depthFormat_;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	// TRANSFER_SRC: double-click picking reads the clicked depth texel back
	// (depth-readback unproject — no CPU vertex copies).
	ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(device_, &ici, nullptr, &depthImage_.image) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(device_, depthImage_.image, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex =
	    modelFindMemoryType(physDevice_, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (vkAllocateMemory(device_, &ai, nullptr, &depthImage_.memory) != VK_SUCCESS) {
		return false;
	}
	vkBindImageMemory(device_, depthImage_.image, depthImage_.memory, 0);
	VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = depthImage_.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = depthFormat_;
	vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
	if (vkCreateImageView(device_, &vci, nullptr, &depthImage_.view) != VK_SUCCESS) {
		return false;
	}
	depthImage_.width = w;
	depthImage_.height = h;

	VkImageView fbViews[2] = {colorImage_.view, depthImage_.view};
	VkFramebufferCreateInfo fbci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	fbci.renderPass = renderPass_;
	fbci.attachmentCount = 2;
	fbci.pAttachments = fbViews;
	fbci.width = w;
	fbci.height = h;
	fbci.layers = 1;
	return vkCreateFramebuffer(device_, &fbci, nullptr, &framebuffer_) == VK_SUCCESS;
}

bool
TileRenderer::createPipeline()
{
	// Set 0: one combined image sampler (base color), fragment stage.
	VkDescriptorSetLayoutBinding b0 = {};
	b0.binding = 0;
	b0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b0.descriptorCount = 1;
	b0.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo slci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	slci.bindingCount = 1;
	slci.pBindings = &b0;
	if (vkCreateDescriptorSetLayout(device_, &slci, nullptr, &setLayout_) != VK_SUCCESS) {
		return false;
	}

	VkPushConstantRange pcr = {};
	pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pcr.offset = 0;
	pcr.size = sizeof(PushBlock); // 80 B — within the 128 B minimum
	VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout_;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipelineLayout_) != VK_SUCCESS) {
		return false;
	}

	auto makeShader = [&](const uint32_t *code, size_t bytes) {
		VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		ci.codeSize = bytes;
		ci.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(device_, &ci, nullptr, &m);
		return m;
	};
	VkShaderModule vert = makeShader(tile_vert_data, sizeof(tile_vert_data));
	VkShaderModule frag = makeShader(tile_frag_data, sizeof(tile_frag_data));
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
		return false;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	VkVertexInputBindingDescription bind = {0, sizeof(TileVertex), VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription attrs[2] = {
	    {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TileVertex, pos)},
	    {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TileVertex, uv)},
	};
	VkPipelineVertexInputStateCreateInfo vi = {
	    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 2;
	vi.pVertexAttributeDescriptions = attrs;

	VkPipelineInputAssemblyStateCreateInfo ia = {
	    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs = {
	    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	// No culling: P3DT photogrammetry has mixed winding + skirt geometry, and
	// the negative-height viewport flips winding anyway.
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms = {
	    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo ds = {
	    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_TRUE;
	ds.depthCompareOp = VK_COMPARE_OP_LESS;

	VkPipelineColorBlendAttachmentState cba = {};
	cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo cb = {
	    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;

	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dync = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dync.dynamicStateCount = 2;
	dync.pDynamicStates = dyn;

	VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pci.stageCount = 2;
	pci.pStages = stages;
	pci.pVertexInputState = &vi;
	pci.pInputAssemblyState = &ia;
	pci.pViewportState = &vp;
	pci.pRasterizationState = &rs;
	pci.pMultisampleState = &ms;
	pci.pDepthStencilState = &ds;
	pci.pColorBlendState = &cb;
	pci.pDynamicState = &dync;
	pci.layout = pipelineLayout_;
	pci.renderPass = renderPass_;
	VkResult res = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline_);

	vkDestroyShaderModule(device_, vert, nullptr);
	vkDestroyShaderModule(device_, frag, nullptr);
	if (res != VK_SUCCESS) {
		return false;
	}

	// Descriptor pool: one set per live tile texture. FREE bit so free()
	// returns sets individually as tiles expire. ~70 visible tiles × a few
	// textures leaves lots of headroom at 4096.
	VkDescriptorPoolSize psize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096};
	VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	dpci.maxSets = 4096;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &psize;
	return vkCreateDescriptorPool(device_, &dpci, nullptr, &descPool_) == VK_SUCCESS;
}

bool
TileRenderer::createSamplerAndDefaults()
{
	VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.maxLod = VK_LOD_CLAMP_NONE;
	if (vkCreateSampler(device_, &sci, nullptr, &sampler_) != VK_SUCCESS) {
		return false;
	}
	const uint8_t white[4] = {255, 255, 255, 255};
	whiteTex_ = uploadTexture(white, 1, 1);
	if (whiteTex_.view == VK_NULL_HANDLE) {
		return false;
	}
	whiteSet_ = allocateTextureSet(whiteTex_.view);
	return whiteSet_ != VK_NULL_HANDLE;
}

ModelImage
TileRenderer::uploadTexture(const uint8_t *rgba, uint32_t w, uint32_t h)
{
	ModelImage img;
	if (!rgba || w == 0 || h == 0) {
		return img;
	}
	img.width = w;
	img.height = h;
	const uint32_t mips = (uint32_t)std::floor(std::log2((float)std::max(w, h))) + 1u;

	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	// SRGB sampled: HW decodes to linear in the shader; the SRGB attachment
	// re-encodes on write (INV-4.6 round-trip, no manual gamma anywhere).
	ici.format = VK_FORMAT_R8G8B8A8_SRGB;
	ici.extent = {w, h, 1};
	ici.mipLevels = mips;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	            VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // SRC for mip blits
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(device_, &ici, nullptr, &img.image) != VK_SUCCESS) {
		return ModelImage{};
	}

	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(device_, img.image, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex =
	    modelFindMemoryType(physDevice_, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (vkAllocateMemory(device_, &ai, nullptr, &img.memory) != VK_SUCCESS) {
		vkDestroyImage(device_, img.image, nullptr);
		return ModelImage{};
	}
	vkBindImageMemory(device_, img.image, img.memory, 0);

	VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = img.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = ici.format;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
	vkCreateImageView(device_, &vci, nullptr, &img.view);

	ModelBuffer staging = modelCreateBuffer(device_, physDevice_, (VkDeviceSize)w * h * 4,
	                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	void *mapped = nullptr;
	vkMapMemory(device_, staging.memory, 0, (VkDeviceSize)w * h * 4, 0, &mapped);
	std::memcpy(mapped, rgba, (size_t)w * h * 4);
	vkUnmapMemory(device_, staging.memory);

	VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cbai.commandPool = cmdPool_;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cmd;
	vkAllocateCommandBuffers(device_, &cbai, &cmd);
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);

	auto barrier = [&](uint32_t mip, uint32_t count, VkImageLayout oldL, VkImageLayout newL,
	                   VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS,
	                   VkPipelineStageFlags dstS) {
		VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.oldLayout = oldL;
		b.newLayout = newL;
		b.srcAccessMask = srcA;
		b.dstAccessMask = dstA;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = img.image;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, count, 0, 1};
		vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
	};

	barrier(0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	        VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {w, h, 1};
	vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
	                       &region);

	int32_t mw = (int32_t)w, mh = (int32_t)h;
	for (uint32_t i = 1; i < mips; ++i) {
		barrier(i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		        VK_PIPELINE_STAGE_TRANSFER_BIT);
		barrier(i, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		        VK_PIPELINE_STAGE_TRANSFER_BIT);
		int32_t nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
		VkImageBlit blit = {};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
		blit.srcOffsets[1] = {mw, mh, 1};
		blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
		blit.dstOffsets[1] = {nw, nh, 1};
		vkCmdBlitImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image,
		               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
		barrier(i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
		        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		mw = nw;
		mh = nh;
	}
	barrier(mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
	        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(queue_);
	vkFreeCommandBuffers(device_, cmdPool_, 1, &cmd);
	modelDestroyBuffer(device_, staging);
	return img;
}

VkDescriptorSet
TileRenderer::allocateTextureSet(VkImageView view)
{
	VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dsai.descriptorPool = descPool_;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &setLayout_;
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (vkAllocateDescriptorSets(device_, &dsai, &set) != VK_SUCCESS) {
		return VK_NULL_HANDLE; // pool exhausted — draw falls back to white
	}
	VkDescriptorImageInfo dii = {sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = set;
	w.dstBinding = 0;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &dii;
	vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
	return set;
}

// ── Per-eye draw (mirrors ModelRenderer::renderEye) ──────────────────────

void
TileRenderer::renderEye(VkImage swapchainImage,
                        VkFormat swapchainFormat,
                        uint32_t imageWidth,
                        uint32_t imageHeight,
                        uint32_t viewportX,
                        uint32_t viewportY,
                        uint32_t viewportWidth,
                        uint32_t viewportHeight,
                        const float viewMatrix[16],
                        const float projMatrix[16],
                        const std::vector<DrawItem> &drawList)
{
	if (!initialized_) {
		return;
	}
	if (imageWidth != width_ || imageHeight != height_) {
		if (!ensureTargets(imageWidth, imageHeight)) {
			return;
		}
	}

	const glm::mat4 view = glm::make_mat4(viewMatrix);
	const glm::mat4 proj = glm::make_mat4(projMatrix);
	const glm::mat4 vp = proj * view;

	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = cmdPool_;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd;
	vkAllocateCommandBuffers(device_, &ai, &cmd);
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);

	VkClearValue clears[2];
	clears[0].color = {{kSkyColor[0], kSkyColor[1], kSkyColor[2], kSkyColor[3]}};
	clears[1].depthStencil = {1.0f, 0};

	VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	rpbi.renderPass = renderPass_;
	rpbi.framebuffer = framebuffer_;
	rpbi.renderArea.offset = {0, 0};
	rpbi.renderArea.extent = {viewportWidth, viewportHeight};
	rpbi.clearValueCount = 2;
	rpbi.pClearValues = clears;
	vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

	// Negative-height viewport flips Vulkan Y-down at the rasterizer (same
	// convention as ModelRenderer — the shell's view/proj are +Y-up).
	VkViewport vpRect = {0.0f,      (float)viewportHeight,  (float)viewportWidth,
	                     -(float)viewportHeight, 0.0f,      1.0f};
	VkRect2D scissor = {{0, 0}, {viewportWidth, viewportHeight}};
	vkCmdSetViewport(cmd, 0, 1, &vpRect);
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	if (!drawList.empty()) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
		for (const DrawItem &item : drawList) {
			const TileGpu *gpu = item.gpu;
			if (!gpu || gpu->vbuf.buffer == VK_NULL_HANDLE) {
				continue;
			}
			PushBlock pb;
			const glm::mat4 model = glm::make_mat4(item.model);
			const glm::mat4 mvp = vp * model;
			std::memcpy(pb.mvp, glm::value_ptr(mvp), sizeof(pb.mvp));

			VkDeviceSize voff = 0;
			vkCmdBindVertexBuffers(cmd, 0, 1, &gpu->vbuf.buffer, &voff);
			vkCmdBindIndexBuffer(cmd, gpu->ibuf.buffer, 0, VK_INDEX_TYPE_UINT32);

			for (const StagedPrimitive &p : gpu->prims) {
				pb.tint[0] = p.tint[0];
				pb.tint[1] = p.tint[1];
				pb.tint[2] = p.tint[2];
				pb.tint[3] = 1.0f;
				vkCmdPushConstants(cmd, pipelineLayout_,
				                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				                   0, sizeof(PushBlock), &pb);
				VkDescriptorSet set =
				    (p.textureIndex >= 0 && p.textureIndex < (int)gpu->texSets.size() &&
				     gpu->texSets[(size_t)p.textureIndex] != VK_NULL_HANDLE)
				        ? gpu->texSets[(size_t)p.textureIndex]
				        : whiteSet_;
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				                        pipelineLayout_, 0, 1, &set, 0, nullptr);
				vkCmdDrawIndexed(cmd, p.indexCount, 1, p.firstIndex, p.vertexOffset, 0);
			}
		}
	}

	vkCmdEndRenderPass(cmd);
	// colorImage_ now TRANSFER_SRC (render-pass finalLayout).

	// Swapchain → TRANSFER_DST. First eye (vpX==0 && vpY==0): UNDEFINED ok;
	// later tiles preserve earlier ones.
	const bool firstEye = (viewportX == 0 && viewportY == 0);
	VkImageMemoryBarrier toDst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	toDst.srcAccessMask = firstEye ? 0 : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toDst.oldLayout = firstEye ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.image = swapchainImage;
	toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
	                     0, nullptr, 0, nullptr, 1, &toDst);

	VkImageBlit blit = {};
	blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.srcOffsets[0] = {0, 0, 0};
	blit.srcOffsets[1] = {(int32_t)viewportWidth, (int32_t)viewportHeight, 1};
	blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.dstOffsets[0] = {(int32_t)viewportX, (int32_t)viewportY, 0};
	blit.dstOffsets[1] = {(int32_t)(viewportX + viewportWidth),
	                      (int32_t)(viewportY + viewportHeight), 1};
	vkCmdBlitImage(cmd, colorImage_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchainImage,
	               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

	VkImageMemoryBarrier toColor = toDst;
	toColor.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
	                     1, &toColor);

	vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(queue_);
	vkFreeCommandBuffers(device_, cmdPool_, 1, &cmd);
}

float
TileRenderer::readDepth(uint32_t px, uint32_t py)
{
	if (!initialized_ || px >= width_ || py >= height_) {
		return 1.0f;
	}
	ModelBuffer readback = modelCreateBuffer(device_, physDevice_, sizeof(float),
	                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (readback.buffer == VK_NULL_HANDLE) {
		return 1.0f;
	}

	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = cmdPool_;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd;
	vkAllocateCommandBuffers(device_, &ai, &cmd);
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);

	VkImageMemoryBarrier toSrc = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	toSrc.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	toSrc.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toSrc.image = depthImage_.image;
	toSrc.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
	region.imageOffset = {(int32_t)px, (int32_t)py, 0};
	region.imageExtent = {1, 1, 1};
	vkCmdCopyImageToBuffer(cmd, depthImage_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       readback.buffer, 1, &region);

	VkImageMemoryBarrier back = toSrc;
	back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	back.dstAccessMask =
	    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	back.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1,
	                     &back);

	vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(queue_);
	vkFreeCommandBuffers(device_, cmdPool_, 1, &cmd);

	float depth = 1.0f;
	void *mapped = nullptr;
	vkMapMemory(device_, readback.memory, 0, sizeof(float), 0, &mapped);
	std::memcpy(&depth, mapped, sizeof(float));
	vkUnmapMemory(device_, readback.memory);
	modelDestroyBuffer(device_, readback);
	return depth;
}

void
TileRenderer::dumpColorTarget(const char *path, uint32_t w, uint32_t h)
{
	if (!initialized_ || colorImage_.image == VK_NULL_HANDLE) {
		return;
	}
	w = std::min(w, width_);
	h = std::min(h, height_);
	const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
	ModelBuffer rb = modelCreateBuffer(device_, physDevice_, bytes,
	                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (rb.buffer == VK_NULL_HANDLE) {
		return;
	}

	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = cmdPool_;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd;
	vkAllocateCommandBuffers(device_, &ai, &cmd);
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);

	// colorImage_ rests in TRANSFER_SRC after a render pass; copy its top-left
	// w×h directly.
	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {w, h, 1};
	vkCmdCopyImageToBuffer(cmd, colorImage_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer,
	                       1, &region);

	vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(queue_);
	vkFreeCommandBuffers(device_, cmdPool_, 1, &cmd);

	void *mapped = nullptr;
	vkMapMemory(device_, rb.memory, 0, bytes, 0, &mapped);
	stbi_write_png(path, (int)w, (int)h, 4, mapped, (int)w * 4);
	vkUnmapMemory(device_, rb.memory);
	modelDestroyBuffer(device_, rb);
	std::fprintf(stderr, "TileRenderer: dumped %ux%u -> %s\n", w, h, path);
}

// ── Teardown ─────────────────────────────────────────────────────────────

void
TileRenderer::cleanup()
{
	if (device_ == VK_NULL_HANDLE) {
		return;
	}
	vkDeviceWaitIdle(device_);

	// Tiles should already be gone (TileEngine/Tileset destroyed first —
	// its destructor free()s each). Sweep any stragglers defensively.
	for (TileGpu *gpu : liveTiles_) {
		for (VkDescriptorSet set : gpu->texSets) {
			if (set != VK_NULL_HANDLE) {
				vkFreeDescriptorSets(device_, descPool_, 1, &set);
			}
		}
		for (auto &img : gpu->textures) {
			if (img.image != VK_NULL_HANDLE) {
				modelDestroyImage(device_, img);
			}
		}
		if (gpu->vbuf.buffer != VK_NULL_HANDLE) {
			modelDestroyBuffer(device_, gpu->vbuf);
		}
		if (gpu->ibuf.buffer != VK_NULL_HANDLE) {
			modelDestroyBuffer(device_, gpu->ibuf);
		}
		delete gpu;
	}
	liveTiles_.clear();
	gpuBytes_ = 0;

	if (whiteTex_.image != VK_NULL_HANDLE) {
		modelDestroyImage(device_, whiteTex_);
	}
	if (sampler_ != VK_NULL_HANDLE) {
		vkDestroySampler(device_, sampler_, nullptr);
		sampler_ = VK_NULL_HANDLE;
	}
	if (descPool_ != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(device_, descPool_, nullptr);
		descPool_ = VK_NULL_HANDLE;
	}
	if (pipeline_ != VK_NULL_HANDLE) {
		vkDestroyPipeline(device_, pipeline_, nullptr);
		pipeline_ = VK_NULL_HANDLE;
	}
	if (pipelineLayout_ != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
		pipelineLayout_ = VK_NULL_HANDLE;
	}
	if (setLayout_ != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
		setLayout_ = VK_NULL_HANDLE;
	}
	if (framebuffer_ != VK_NULL_HANDLE) {
		vkDestroyFramebuffer(device_, framebuffer_, nullptr);
		framebuffer_ = VK_NULL_HANDLE;
	}
	if (colorImage_.image != VK_NULL_HANDLE) {
		modelDestroyImage(device_, colorImage_);
	}
	if (depthImage_.image != VK_NULL_HANDLE) {
		modelDestroyImage(device_, depthImage_);
	}
	if (renderPass_ != VK_NULL_HANDLE) {
		vkDestroyRenderPass(device_, renderPass_, nullptr);
		renderPass_ = VK_NULL_HANDLE;
	}
	if (cmdPool_ != VK_NULL_HANDLE) {
		vkDestroyCommandPool(device_, cmdPool_, nullptr);
		cmdPool_ = VK_NULL_HANDLE;
	}
	initialized_ = false;
	// Null the device so a second cleanup() (static destructor at exit, after
	// main() destroyed the VkDevice) is a no-op instead of an abort.
	device_ = VK_NULL_HANDLE;
	queue_ = VK_NULL_HANDLE;
	physDevice_ = VK_NULL_HANDLE;
	instance_ = VK_NULL_HANDLE;
}

TileRenderer::~TileRenderer()
{
	cleanup();
}
