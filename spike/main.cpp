// EarthView M0 spike — headless Google Photorealistic 3D Tiles → PNG.
//
// Proves the full chain with zero GPU: CesiumCurl accessor → root.json
// (key + session) → tile selection (SSE) → Draco/glTF/JPEG decode →
// software-rasterize the selected tiles over Paris → /tmp/earthview_m0.png,
// plus the aggregated data-attribution credits on stdout.
//
// Key supply: GOOGLE_MAPS_API_KEY env var, or `key=...` in earthview.ini
// (repo root or cwd).

#include <Cesium3DTilesContent/registerAllTileContentTypes.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/ViewState.h>
#include <Cesium3DTilesSelection/ViewUpdateResult.h>
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/IAssetAccessor.h>
#include <CesiumAsync/IAssetRequest.h>
#include <CesiumAsync/ITaskProcessor.h>
#include <CesiumCurl/CurlAssetAccessor.h>
#include <CesiumGeospatial/Cartographic.h>
#include <CesiumGeospatial/Ellipsoid.h>
#include <CesiumGltf/AccessorView.h>
#include <CesiumGltf/Model.h>
#include <CesiumUtility/CreditSystem.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <spdlog/spdlog.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 800;
constexpr double kHFovDeg = 60.0;
// Eiffel Tower, looking from the SE at ~35° tilt.
constexpr double kTargetLonDeg = 2.2945;
constexpr double kTargetLatDeg = 48.8584;
constexpr double kCamDistance = 2500.0; // m from target
constexpr double kCamElevDeg = 35.0;    // above horizon
constexpr int kSettleFrames = 5;
constexpr int kTimeoutSec = 120;
constexpr double kDioramaScale = 1.0 / 3000.0; // PRD §6.1 default tabletop scale

class ThreadTaskProcessor : public CesiumAsync::ITaskProcessor
{
public:
	void
	startTask(std::function<void()> f) override
	{
		std::thread(std::move(f)).detach();
	}
};

// cesium-native propagates the root URL's `key` query param to every child
// request for Google P3DT, so a bare CurlAssetAccessor is sufficient. This
// decorator is a belt-and-suspenders safeguard: it re-appends `key=...` to any
// tile.googleapis.com URL that somehow arrives without one (e.g. a child URI
// whose own query string replaced the base query during resolution).
class KeyInjectingAccessor : public CesiumAsync::IAssetAccessor
{
public:
	KeyInjectingAccessor(std::shared_ptr<CesiumAsync::IAssetAccessor> inner, std::string key)
	    : _inner(std::move(inner)), _key(std::move(key))
	{
	}

	CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>>
	get(const CesiumAsync::AsyncSystem &asyncSystem, const std::string &url,
	    const std::vector<THeader> &headers = {}) override
	{
		return _inner->get(asyncSystem, withKey(url), headers);
	}

	CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>>
	request(const CesiumAsync::AsyncSystem &asyncSystem, const std::string &verb,
	        const std::string &url, const std::vector<THeader> &headers = {},
	        const std::span<const std::byte> &payload = {}) override
	{
		return _inner->request(asyncSystem, verb, withKey(url), headers, payload);
	}

	void
	tick() noexcept override
	{
		_inner->tick();
	}

private:
	std::string
	withKey(const std::string &url) const
	{
		if (url.find("tile.googleapis.com") == std::string::npos ||
		    url.find("key=") != std::string::npos) {
			return url;
		}
		return url + (url.find('?') == std::string::npos ? "?key=" : "&key=") + _key;
	}

	std::shared_ptr<CesiumAsync::IAssetAccessor> _inner;
	std::string _key;
};

std::string
getApiKey()
{
	if (const char *env = std::getenv("GOOGLE_MAPS_API_KEY"); env && *env) {
		return env;
	}
	for (const char *path : {"earthview.ini", "../earthview.ini", "../../earthview.ini"}) {
		std::ifstream f(path);
		std::string line;
		while (std::getline(f, line)) {
			if (line.rfind("key=", 0) == 0 && line.size() > 4) {
				// strip trailing whitespace/CR
				std::string key = line.substr(4);
				while (!key.empty() && (key.back() == '\r' || key.back() == ' ' || key.back() == '\n')) {
					key.pop_back();
				}
				return key;
			}
		}
	}
	return {};
}

std::string
stripHtml(const std::string &html)
{
	static const std::regex tags("<[^>]*>");
	return std::regex_replace(html, tags, "");
}

struct Framebuffer
{
	std::vector<uint8_t> color; // RGBA8
	std::vector<float> depth;

	Framebuffer()
	{
		color.resize(size_t(kWidth) * kHeight * 4);
		depth.assign(size_t(kWidth) * kHeight, std::numeric_limits<float>::infinity());
		// sky gradient background
		for (int y = 0; y < kHeight; ++y) {
			float t = float(y) / kHeight;
			uint8_t r = uint8_t(140 + 60 * t), g = uint8_t(170 + 50 * t), b = uint8_t(210 + 40 * t);
			for (int x = 0; x < kWidth; ++x) {
				uint8_t *p = &color[(size_t(y) * kWidth + x) * 4];
				p[0] = r, p[1] = g, p[2] = b, p[3] = 255;
			}
		}
	}
};

struct TextureRef
{
	const std::byte *pixels = nullptr;
	int32_t w = 0, h = 0, channels = 0;

	glm::u8vec3
	sample(glm::vec2 uv) const
	{
		if (!pixels || w <= 0 || h <= 0) {
			return {180, 180, 180};
		}
		float u = uv.x - std::floor(uv.x);
		float v = uv.y - std::floor(uv.y);
		int x = std::min(int(u * w), int(w - 1));
		int y = std::min(int(v * h), int(h - 1));
		const std::byte *p = pixels + (size_t(y) * w + x) * channels;
		auto c = [&](int i) { return channels > i ? uint8_t(p[i]) : uint8_t(p[0]); };
		return {c(0), c(1), c(2)};
	}
};

struct RasterStats
{
	size_t triangles = 0;
	size_t trianglesDrawn = 0;
	size_t primitives = 0;
	size_t texturedPrimitives = 0;
	size_t behind = 0;
	size_t degenerate = 0;
	size_t offscreen = 0;
};

void
rasterizePrimitive(Framebuffer &fb,
                   const glm::dmat4 &viewProj,
                   const glm::dmat4 &world,
                   const CesiumGltf::Model &model,
                   const CesiumGltf::MeshPrimitive &prim,
                   RasterStats &stats)
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
	stats.primitives++;

	// UVs + base color texture (optional — fall back to gray)
	TextureRef tex;
	AccessorView<glm::vec2> uvs;
	if (auto uvIt = prim.attributes.find("TEXCOORD_0"); uvIt != prim.attributes.end()) {
		uvs = AccessorView<glm::vec2>(model, uvIt->second);
	}
	if (prim.material >= 0 && prim.material < int32_t(model.materials.size())) {
		const Material &mat = model.materials[size_t(prim.material)];
		if (mat.pbrMetallicRoughness && mat.pbrMetallicRoughness->baseColorTexture) {
			int32_t texIdx = mat.pbrMetallicRoughness->baseColorTexture->index;
			if (texIdx >= 0 && texIdx < int32_t(model.textures.size())) {
				int32_t src = model.textures[size_t(texIdx)].source;
				if (src >= 0 && src < int32_t(model.images.size()) && model.images[size_t(src)].pAsset) {
					const ImageAsset &img = *model.images[size_t(src)].pAsset;
					if (!img.pixelData.empty() && img.bytesPerChannel == 1) {
						tex.pixels = img.pixelData.data();
						tex.w = img.width;
						tex.h = img.height;
						tex.channels = img.channels;
						stats.texturedPrimitives++;
					}
				}
			}
		}
	}
	const bool hasUVs = uvs.status() == AccessorViewStatus::Valid && tex.pixels;

	// Index fetcher across component types; -1 accessor = non-indexed.
	std::function<uint32_t(int64_t)> indexAt;
	int64_t indexCount = 0;
	AccessorView<uint16_t> idx16;
	AccessorView<uint32_t> idx32;
	AccessorView<uint8_t> idx8;
	if (prim.indices >= 0) {
		idx16 = AccessorView<uint16_t>(model, prim.indices);
		idx32 = AccessorView<uint32_t>(model, prim.indices);
		idx8 = AccessorView<uint8_t>(model, prim.indices);
		if (idx16.status() == AccessorViewStatus::Valid) {
			indexCount = idx16.size();
			indexAt = [&](int64_t i) { return uint32_t(idx16[i]); };
		} else if (idx32.status() == AccessorViewStatus::Valid) {
			indexCount = idx32.size();
			indexAt = [&](int64_t i) { return idx32[i]; };
		} else if (idx8.status() == AccessorViewStatus::Valid) {
			indexCount = idx8.size();
			indexAt = [&](int64_t i) { return uint32_t(idx8[i]); };
		} else {
			return;
		}
	} else {
		indexCount = positions.size();
		indexAt = [](int64_t i) { return uint32_t(i); };
	}

	const glm::dmat4 mvp = viewProj * world;

	for (int64_t t = 0; t + 2 < indexCount; t += 3) {
		stats.triangles++;
		glm::dvec4 clip[3];
		glm::vec2 uv[3] = {{0, 0}, {0, 0}, {0, 0}};
		bool reject = false;
		for (int v = 0; v < 3; ++v) {
			uint32_t i = indexAt(t + v);
			if (i >= uint32_t(positions.size())) {
				reject = true;
				break;
			}
			glm::vec3 p = positions[i];
			clip[v] = mvp * glm::dvec4(p.x, p.y, p.z, 1.0);
			if (clip[v].w < 1.0) { // behind/too close to near plane — skip (spike-grade clipping)
				reject = true;
				break;
			}
			if (hasUVs && i < uint32_t(uvs.size())) {
				uv[v] = uvs[i];
			}
		}
		if (reject) {
			stats.behind++;
			continue;
		}

		// NDC → screen, keep 1/w for perspective-correct UV
		glm::dvec3 s[3];
		double invW[3];
		for (int v = 0; v < 3; ++v) {
			invW[v] = 1.0 / clip[v].w;
			glm::dvec3 ndc = glm::dvec3(clip[v]) * invW[v];
			s[v] = {(ndc.x * 0.5 + 0.5) * kWidth, (0.5 - ndc.y * 0.5) * kHeight, ndc.z};
		}

		double area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
		if (std::abs(area) < 1e-9) {
			stats.degenerate++;
			continue;
		}
		double invArea = 1.0 / area;

		int minX = std::max(0, int(std::floor(std::min({s[0].x, s[1].x, s[2].x}))));
		int maxX = std::min(kWidth - 1, int(std::ceil(std::max({s[0].x, s[1].x, s[2].x}))));
		int minY = std::max(0, int(std::floor(std::min({s[0].y, s[1].y, s[2].y}))));
		int maxY = std::min(kHeight - 1, int(std::ceil(std::max({s[0].y, s[1].y, s[2].y}))));
		if (minX > maxX || minY > maxY) {
			stats.offscreen++;
			continue;
		}
		stats.trianglesDrawn++;

		for (int y = minY; y <= maxY; ++y) {
			for (int x = minX; x <= maxX; ++x) {
				double px = x + 0.5, py = y + 0.5;
				double w0 = ((s[1].x - px) * (s[2].y - py) - (s[2].x - px) * (s[1].y - py)) * invArea;
				double w1 = ((s[2].x - px) * (s[0].y - py) - (s[0].x - px) * (s[2].y - py)) * invArea;
				double w2 = 1.0 - w0 - w1;
				if (w0 < 0 || w1 < 0 || w2 < 0) {
					continue;
				}
				float z = float(w0 * s[0].z + w1 * s[1].z + w2 * s[2].z);
				size_t pix = size_t(y) * kWidth + x;
				if (z >= fb.depth[pix]) {
					continue;
				}
				glm::u8vec3 rgb{170, 170, 170};
				if (hasUVs) {
					double iw = w0 * invW[0] + w1 * invW[1] + w2 * invW[2];
					glm::vec2 uvp = glm::vec2(
					    (w0 * uv[0].x * invW[0] + w1 * uv[1].x * invW[1] + w2 * uv[2].x * invW[2]) / iw,
					    (w0 * uv[0].y * invW[0] + w1 * uv[1].y * invW[1] + w2 * uv[2].y * invW[2]) / iw);
					rgb = tex.sample(uvp);
				}
				fb.depth[pix] = z;
				uint8_t *p = &fb.color[pix * 4];
				p[0] = rgb[0], p[1] = rgb[1], p[2] = rgb[2], p[3] = 255;
			}
		}
	}
}

// glTF Y-up -> ECEF Z-up: (x,y,z) -> (x,-z,y). cesium-native's tile transform
// does not bake this in; premultiply it onto Tile::getTransform().
glm::dmat4
yUpToZUp()
{
	glm::dmat4 m(1.0);
	m[1] = glm::dvec4(0, 0, 1, 0);
	m[2] = glm::dvec4(0, -1, 0, 0);
	return m;
}

struct Camera
{
	glm::dvec3 pos, dir, up;
	double hfov, vfov;
};

// Orbit camera around `target`: azimuth (0 = due north, +east), elevation above
// the local horizon, at `dist` metres.
Camera
buildCamera(const CesiumGeospatial::Ellipsoid &ell,
            const CesiumGeospatial::Cartographic &targetCarto, glm::dvec3 target, double azDeg,
            double elevDeg, double dist)
{
	glm::dvec3 up = ell.geodeticSurfaceNormal(targetCarto);
	glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
	glm::dvec3 north = glm::normalize(glm::cross(up, east));
	double az = glm::radians(azDeg), el = glm::radians(elevDeg);
	glm::dvec3 horiz = glm::normalize(std::cos(az) * north + std::sin(az) * east);
	Camera c;
	c.pos = target + horiz * (dist * std::cos(el)) + up * (dist * std::sin(el));
	c.dir = glm::normalize(target - c.pos);
	c.up = up;
	c.hfov = glm::radians(kHFovDeg);
	c.vfov = 2.0 * std::atan(std::tan(c.hfov / 2.0) * double(kHeight) / double(kWidth));
	return c;
}

// Drive updateView until the selection is quiet AND carries render content.
const Cesium3DTilesSelection::ViewUpdateResult *
settle(Cesium3DTilesSelection::Tileset &tileset, CesiumAsync::AsyncSystem &asyncSystem,
       const Cesium3DTilesSelection::ViewState &view, int timeoutSec, bool verbose)
{
	auto start = std::chrono::steady_clock::now();
	const Cesium3DTilesSelection::ViewUpdateResult *result = nullptr;
	int settledFrames = 0, frame = 0;
	while (true) {
		result = &tileset.updateView({view});
		asyncSystem.dispatchMainThreadTasks();

		int renderTiles = 0;
		for (const auto &pTile : result->tilesToRenderThisFrame) {
			if (pTile->getContent().isRenderContent()) {
				++renderTiles;
			}
		}
		bool quiet = result->workerThreadTileLoadQueueLength == 0 &&
		             result->mainThreadTileLoadQueueLength == 0 && renderTiles > 0;
		settledFrames = quiet ? settledFrames + 1 : 0;
		if (settledFrames >= kSettleFrames) {
			break;
		}
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
		    std::chrono::steady_clock::now() - start);
		if (elapsed.count() > timeoutSec) {
			if (verbose) {
				std::fprintf(stderr, "WARN: settle timeout after %ds\n", timeoutSec);
			}
			break;
		}
		if (verbose && ++frame % 50 == 0) {
			std::printf("  ... %.1f%% loaded, %zu selected\n",
			            double(tileset.computeLoadProgress()),
			            result->tilesToRenderThisFrame.size());
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
	}
	return result;
}

struct SceneOut
{
	RasterStats stats;
	int renderTiles = 0;
};

// Rasterize the selected tiles for `cam` into `fb`. Returns per-frame stats.
SceneOut
renderScene(const Cesium3DTilesSelection::ViewUpdateResult &result, const Camera &cam,
            Framebuffer &fb)
{
	glm::dmat4 viewMat = glm::lookAt(cam.pos, cam.pos + cam.dir, cam.up);
	glm::dmat4 projMat = glm::perspective(cam.vfov, double(kWidth) / double(kHeight), 10.0, 1.0e7);
	glm::dmat4 viewProj = projMat * viewMat;
	glm::dmat4 yz = yUpToZUp();

	SceneOut out;
	for (const auto &pTile : result.tilesToRenderThisFrame) {
		const auto &content = pTile->getContent();
		if (!content.isRenderContent()) {
			continue;
		}
		++out.renderTiles;
		const CesiumGltf::Model &model = content.getRenderContent()->getModel();
		const glm::dmat4 tileTransform = yz * pTile->getTransform();
		model.forEachPrimitiveInScene(
		    -1, [&](const CesiumGltf::Model &m, const CesiumGltf::Node &, const CesiumGltf::Mesh &,
		            const CesiumGltf::MeshPrimitive &prim, const glm::dmat4 &nodeTransform) {
			    rasterizePrimitive(fb, viewProj, tileTransform * nodeTransform, m, prim, out.stats);
		    });
	}
	return out;
}

// Resident-set size in MB (current footprint), for leak/churn checks.
double
currentRssMB()
{
#ifdef __APPLE__
	mach_task_basic_info info{};
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
	              &count) == KERN_SUCCESS) {
		return double(info.resident_size) / (1024.0 * 1024.0);
	}
#endif
	return 0.0;
}

// Test that M1's GPU precision approach holds: project every sampled vertex two
// ways — full double (P*V*world) vs RTC (subtract the anchor in double, cast the
// small remainder to float32, multiply by a float32 anchor-relative matrix) —
// and report the worst screen-space divergence. Also report the diorama-space
// extent at `kDioramaScale`. Anchor is the camera target.
void
measureRtcAndDiorama(const Cesium3DTilesSelection::ViewUpdateResult &result, const Camera &cam,
                     glm::dvec3 anchor)
{
	glm::dmat4 viewMat = glm::lookAt(cam.pos, cam.pos + cam.dir, cam.up);
	glm::dmat4 projMat = glm::perspective(cam.vfov, double(kWidth) / double(kHeight), 10.0, 1.0e7);
	glm::dmat4 viewProj = projMat * viewMat;
	glm::dmat4 yz = yUpToZUp();

	// M1 GPU upload mirror: positions arrive as float anchor-relative, MVP as a
	// float matrix with the big anchor translation folded in (built in double,
	// cast once).
	glm::mat4 viewProjRelF = glm::mat4(viewProj * glm::translate(glm::dmat4(1.0), anchor));

	double maxDivPx = 0.0;
	glm::dvec3 bbMin(1e308), bbMax(-1e308);
	size_t samples = 0;

	for (const auto &pTile : result.tilesToRenderThisFrame) {
		const auto &content = pTile->getContent();
		if (!content.isRenderContent()) {
			continue;
		}
		const CesiumGltf::Model &model = content.getRenderContent()->getModel();
		const glm::dmat4 tileTransform = yz * pTile->getTransform();
		model.forEachPrimitiveInScene(
		    -1, [&](const CesiumGltf::Model &m, const CesiumGltf::Node &, const CesiumGltf::Mesh &,
		            const CesiumGltf::MeshPrimitive &prim, const glm::dmat4 &nodeTransform) {
			    auto it = prim.attributes.find("POSITION");
			    if (it == prim.attributes.end()) {
				    return;
			    }
			    CesiumGltf::AccessorView<glm::vec3> pos(m, it->second);
			    if (pos.status() != CesiumGltf::AccessorViewStatus::Valid) {
				    return;
			    }
			    glm::dmat4 world = tileTransform * nodeTransform;
			    for (int64_t i = 0; i < pos.size(); i += 50) { // sample every 50th vertex
				    glm::vec3 lp = pos[i];
				    glm::dvec4 wp = world * glm::dvec4(lp.x, lp.y, lp.z, 1.0);

				    bbMin = glm::min(bbMin, glm::dvec3(wp) - anchor);
				    bbMax = glm::max(bbMax, glm::dvec3(wp) - anchor);

				    glm::dvec4 clipD = viewProj * wp;
				    if (clipD.w < 1.0) {
					    continue;
				    }
				    glm::dvec3 nd = glm::dvec3(clipD) / clipD.w;
				    glm::dvec2 sD((nd.x * 0.5 + 0.5) * kWidth, (0.5 - nd.y * 0.5) * kHeight);

				    glm::vec3 relF = glm::vec3(wp.x - anchor.x, wp.y - anchor.y, wp.z - anchor.z);
				    glm::vec4 clipF = viewProjRelF * glm::vec4(relF, 1.0f);
				    glm::vec3 nf = glm::vec3(clipF) / clipF.w;
				    glm::dvec2 sF((nf.x * 0.5f + 0.5f) * kWidth, (0.5f - nf.y * 0.5f) * kHeight);

				    maxDivPx = std::max(maxDivPx, glm::length(sD - sF));
				    ++samples;
			    }
		    });
	}

	glm::dvec3 extent = bbMax - bbMin;
	std::printf("--- RTC precision (M1 GPU pipeline proxy) ---\n");
	std::printf("  sampled %zu verts; max double-vs-RTCfloat screen divergence = %.4f px\n", samples,
	            maxDivPx);
	std::printf("  verdict: %s (threshold 0.5px)\n", maxDivPx < 0.5 ? "PASS — float32 RTC is safe"
	                                                                 : "FAIL — needs tighter scheme");
	std::printf("--- Diorama transform (scale 1:%.0f) ---\n", 1.0 / kDioramaScale);
	std::printf("  anchor-relative extent: %.0f x %.0f x %.0f m\n", extent.x, extent.y, extent.z);
	std::printf("  at diorama scale:      %.3f x %.3f x %.3f m (display-volume sized)\n",
	            extent.x * kDioramaScale, extent.y * kDioramaScale, extent.z * kDioramaScale);
}

int
runStatic(Cesium3DTilesSelection::Tileset &tileset, CesiumAsync::AsyncSystem &asyncSystem,
          const CesiumGeospatial::Ellipsoid &ell,
          const CesiumGeospatial::Cartographic &targetCarto, glm::dvec3 target,
          std::shared_ptr<CesiumUtility::CreditSystem> pCredits)
{
	using namespace Cesium3DTilesSelection;
	Camera cam = buildCamera(ell, targetCarto, target, /*az*/ 158.0, kCamElevDeg, kCamDistance);
	ViewState view(cam.pos, cam.dir, cam.up, glm::dvec2(kWidth, kHeight), cam.hfov, cam.vfov, ell);

	std::printf("EarthView M0: streaming Paris (%.4f, %.4f) ...\n", kTargetLatDeg, kTargetLonDeg);
	auto t0 = std::chrono::steady_clock::now();
	const ViewUpdateResult *result = settle(tileset, asyncSystem, view, kTimeoutSec, true);
	double secs = std::chrono::duration_cast<std::chrono::milliseconds>(
	                  std::chrono::steady_clock::now() - t0)
	                  .count() /
	              1000.0;

	Framebuffer fb;
	SceneOut scene = renderScene(*result, cam, fb);
	std::printf("Selection settled: %d render tiles in %.1fs\n", scene.renderTiles, secs);

	const char *outPath = "/tmp/earthview_m0.png";
	stbi_write_png(outPath, kWidth, kHeight, 4, fb.color.data(), kWidth * 4);
	std::printf("Wrote %s\n", outPath);
	std::printf("Stats: %zu primitives (%zu textured), %zu tris, %zu drawn "
	            "(behind=%zu degen=%zu offscreen=%zu)\n",
	            scene.stats.primitives, scene.stats.texturedPrimitives, scene.stats.triangles,
	            scene.stats.trianglesDrawn, scene.stats.behind, scene.stats.degenerate,
	            scene.stats.offscreen);

	measureRtcAndDiorama(*result, cam, target);

	std::printf("--- Data attribution ---\n");
	for (const auto &credit : pCredits->getSnapshot().currentCredits) {
		std::printf("  %s\n", stripHtml(pCredits->getHtml(credit)).c_str());
	}

	bool ok = scene.stats.trianglesDrawn > 0;
	std::printf(ok ? "M0 PASS\n" : "M0 FAIL: nothing rasterized\n");
	return ok ? 0 : 1;
}

// Orbit the camera around Paris over a full revolution, settling + rendering
// each step, to exercise continuous tile load/unload churn (the M1 navigation
// case). Reports tile count + RSS per step to check for crashes / unbounded
// memory, and dumps a few frames.
int
runOrbit(Cesium3DTilesSelection::Tileset &tileset, CesiumAsync::AsyncSystem &asyncSystem,
         const CesiumGeospatial::Ellipsoid &ell,
         const CesiumGeospatial::Cartographic &targetCarto, glm::dvec3 target)
{
	using namespace Cesium3DTilesSelection;
	constexpr int kPerRev = 12; // 30 deg apart
	constexpr int kRevs = 3;    // revisit viewpoints to distinguish cache-cap from leak
	constexpr int kSteps = kPerRev * kRevs;
	std::printf("EarthView M0 orbit: %d revolutions (%d frames) around Paris "
	            "(tile-churn + memory)\n",
	            kRevs, kSteps);
	std::printf(" step  rev   az   renderTiles   RSS(MB)\n");

	double rssStart = currentRssMB(), rssMax = rssStart, rssAfterRev1 = 0;
	int minTiles = 1 << 30, maxTiles = 0;
	for (int s = 0; s < kSteps; ++s) {
		double az = 360.0 * (s % kPerRev) / kPerRev;
		Camera cam = buildCamera(ell, targetCarto, target, az, kCamElevDeg, kCamDistance);
		ViewState view(cam.pos, cam.dir, cam.up, glm::dvec2(kWidth, kHeight), cam.hfov, cam.vfov,
		               ell);
		// Per-step settle budget is short: most tiles are already resident from
		// neighbouring views, so this measures incremental churn, not cold load.
		const ViewUpdateResult *result = settle(tileset, asyncSystem, view, 20, false);

		Framebuffer fb;
		SceneOut scene = renderScene(*result, cam, fb);
		double rss = currentRssMB();
		rssMax = std::max(rssMax, rss);
		minTiles = std::min(minTiles, scene.renderTiles);
		maxTiles = std::max(maxTiles, scene.renderTiles);
		if (s == kPerRev - 1) {
			rssAfterRev1 = rss;
		}
		std::printf("  %3d  %3d  %4.0f      %5d       %6.1f\n", s, s / kPerRev, az,
		            scene.renderTiles, rss);

		if (s == 0 || s == kPerRev / 2) {
			char path[64];
			std::snprintf(path, sizeof(path), "/tmp/earthview_orbit_%02d.png", s);
			stbi_write_png(path, kWidth, kHeight, 4, fb.color.data(), kWidth * 4);
			std::printf("        wrote %s\n", path);
		}
	}

	// After rev 1 the camera revisits viewpoints, so any further climb is new
	// allocation, not re-traversal. Plateau across revs 2-3 = cache-cap (healthy);
	// continued linear climb = leak.
	double rssEnd = currentRssMB();
	double laterGrowth = rssEnd - rssAfterRev1;
	std::printf("Churn: render tiles ranged %d..%d across the orbit\n", minTiles, maxTiles);
	std::printf("Memory: start %.1f, end-of-rev1 %.1f, peak %.1f, final %.1f MB\n", rssStart,
	            rssAfterRev1, rssMax, rssEnd);
	std::printf("Memory growth after rev 1 (revisiting views): %.1f MB\n", laterGrowth);
	bool plateaued = laterGrowth < 80.0; // re-seen views shouldn't keep allocating
	std::printf(plateaued ? "  -> plateaued: cache-capped, no leak\n"
	                      : "  -> still climbing: investigate cache cap / leak\n");
	bool ok = maxTiles > 0 && plateaued;
	std::printf(ok ? "ORBIT PASS — no crash, memory bounded by cache cap\n"
	               : "ORBIT FAIL — memory not bounded\n");
	return ok ? 0 : 1;
}

} // namespace

int
main(int argc, char **argv)
{
	using namespace Cesium3DTilesSelection;
	using namespace CesiumGeospatial;

	const bool orbitMode = argc > 1 && std::string(argv[1]) == "orbit";

	const std::string key = getApiKey();
	if (key.empty()) {
		std::fprintf(stderr,
		             "ERROR: no API key. Set GOOGLE_MAPS_API_KEY or put `key=...` in earthview.ini\n"
		             "(Google Cloud Console -> enable \"Map Tiles API\" -> create API key)\n");
		return 2;
	}

	spdlog::set_level(spdlog::level::critical); // mute per-request parse-warning spam

	// REQUIRED: register glb/b3dm/etc converters with GltfConverters. Without
	// this the converter registry is empty, every glb tile falls through to the
	// JSON tileset parser, and geometry silently never loads.
	Cesium3DTilesContent::registerAllTileContentTypes();

	CesiumAsync::AsyncSystem asyncSystem(std::make_shared<ThreadTaskProcessor>());
	auto pAccessor = std::make_shared<KeyInjectingAccessor>(
	    std::make_shared<CesiumCurl::CurlAssetAccessor>(), key);
	auto pCredits = std::make_shared<CesiumUtility::CreditSystem>();

	TilesetExternals externals{pAccessor, nullptr, asyncSystem, pCredits};

	TilesetOptions options;
	options.maximumScreenSpaceError = 16.0;
	options.showCreditsOnScreen = true;

	const std::string url = "https://tile.googleapis.com/v1/3dtiles/root.json?key=" + key;
	Tileset tileset(externals, url, options);

	const Ellipsoid &ell = Ellipsoid::WGS84;
	Cartographic targetCarto = Cartographic::fromDegrees(kTargetLonDeg, kTargetLatDeg, 60.0);
	glm::dvec3 target = ell.cartographicToCartesian(targetCarto);

	return orbitMode ? runOrbit(tileset, asyncSystem, ell, targetCarto, target)
	                 : runStatic(tileset, asyncSystem, ell, targetCarto, target, pCredits);
}
