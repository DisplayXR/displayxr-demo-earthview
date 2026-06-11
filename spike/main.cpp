// EarthView M0 spike — headless Google Photorealistic 3D Tiles → PNG.
//
// Proves the full chain with zero GPU: CesiumCurl accessor → root.json
// (key + session) → tile selection (SSE) → Draco/glTF/JPEG decode →
// software-rasterize the selected tiles over Paris → /tmp/earthview_m0.png,
// plus the aggregated data-attribution credits on stdout.
//
// Key supply: GOOGLE_MAPS_API_KEY env var, or `key=...` in earthview.ini
// (repo root or cwd).

#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/ViewState.h>
#include <Cesium3DTilesSelection/ViewUpdateResult.h>
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/ITaskProcessor.h>
#include <CesiumCurl/CurlAssetAccessor.h>
#include <CesiumGeospatial/Cartographic.h>
#include <CesiumGeospatial/Ellipsoid.h>
#include <CesiumGltf/AccessorView.h>
#include <CesiumGltf/Model.h>
#include <CesiumUtility/CreditSystem.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

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
constexpr int kTimeoutSec = 180;

class ThreadTaskProcessor : public CesiumAsync::ITaskProcessor
{
public:
	void
	startTask(std::function<void()> f) override
	{
		std::thread(std::move(f)).detach();
	}
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
			continue;
		}
		double invArea = 1.0 / area;

		int minX = std::max(0, int(std::floor(std::min({s[0].x, s[1].x, s[2].x}))));
		int maxX = std::min(kWidth - 1, int(std::ceil(std::max({s[0].x, s[1].x, s[2].x}))));
		int minY = std::max(0, int(std::floor(std::min({s[0].y, s[1].y, s[2].y}))));
		int maxY = std::min(kHeight - 1, int(std::ceil(std::max({s[0].y, s[1].y, s[2].y}))));
		if (minX > maxX || minY > maxY) {
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

} // namespace

int
main()
{
	using namespace Cesium3DTilesSelection;
	using namespace CesiumGeospatial;

	const std::string key = getApiKey();
	if (key.empty()) {
		std::fprintf(stderr,
		             "ERROR: no API key. Set GOOGLE_MAPS_API_KEY or put `key=...` in earthview.ini\n"
		             "(Google Cloud Console -> enable \"Map Tiles API\" -> create API key)\n");
		return 2;
	}

	CesiumAsync::AsyncSystem asyncSystem(std::make_shared<ThreadTaskProcessor>());
	auto pAccessor = std::make_shared<CesiumCurl::CurlAssetAccessor>();
	auto pCredits = std::make_shared<CesiumUtility::CreditSystem>();

	TilesetExternals externals{pAccessor, nullptr, asyncSystem, pCredits};

	TilesetOptions options;
	options.maximumScreenSpaceError = 16.0;
	options.showCreditsOnScreen = true;

	const std::string url = "https://tile.googleapis.com/v1/3dtiles/root.json?key=" + key;
	Tileset tileset(externals, url, options);

	// Camera: SE of the Eiffel Tower, elevated, looking at it.
	const Ellipsoid &ell = Ellipsoid::WGS84;
	Cartographic targetCarto = Cartographic::fromDegrees(kTargetLonDeg, kTargetLatDeg, 60.0);
	glm::dvec3 target = ell.cartographicToCartesian(targetCarto);
	glm::dvec3 up = ell.geodeticSurfaceNormal(targetCarto);
	// local ENU at target
	glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
	glm::dvec3 north = glm::normalize(glm::cross(up, east));
	glm::dvec3 horiz = glm::normalize(-north + east * 0.4); // approach from SSW-ish
	double elev = glm::radians(kCamElevDeg);
	glm::dvec3 camPos =
	    target + horiz * (kCamDistance * std::cos(elev)) + up * (kCamDistance * std::sin(elev));
	glm::dvec3 dir = glm::normalize(target - camPos);

	double hfov = glm::radians(kHFovDeg);
	double vfov = 2.0 * std::atan(std::tan(hfov / 2.0) * double(kHeight) / double(kWidth));
	ViewState view(camPos, dir, up, glm::dvec2(kWidth, kHeight), hfov, vfov, ell);

	std::printf("EarthView M0: streaming Paris (%.4f, %.4f) ...\n", kTargetLatDeg, kTargetLonDeg);

	auto start = std::chrono::steady_clock::now();
	const ViewUpdateResult *result = nullptr;
	int settled = 0;
	int frame = 0;
	while (true) {
		result = &tileset.updateView({view});
		asyncSystem.dispatchMainThreadTasks();

		bool quiet = result->workerThreadTileLoadQueueLength == 0 &&
		             result->mainThreadTileLoadQueueLength == 0 &&
		             !result->tilesToRenderThisFrame.empty();
		settled = quiet ? settled + 1 : 0;
		if (settled >= kSettleFrames) {
			break;
		}

		auto elapsed =
		    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);
		if (elapsed.count() > kTimeoutSec) {
			std::fprintf(stderr, "WARN: timeout after %ds — rendering what we have\n", kTimeoutSec);
			break;
		}
		if (++frame % 50 == 0) {
			std::printf("  ... %.1f%% loaded, %zu tiles selected, queues w=%d m=%d\n",
			            double(tileset.computeLoadProgress()),
			            result->tilesToRenderThisFrame.size(),
			            int(result->workerThreadTileLoadQueueLength),
			            int(result->mainThreadTileLoadQueueLength));
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
	}

	auto loadSecs = std::chrono::duration_cast<std::chrono::milliseconds>(
	                    std::chrono::steady_clock::now() - start)
	                    .count() /
	                1000.0;
	std::printf("Selection settled: %zu tiles in %.1fs\n", result->tilesToRenderThisFrame.size(),
	            loadSecs);

	// Rasterize
	glm::dmat4 viewMat = glm::lookAt(camPos, camPos + dir, up);
	glm::dmat4 projMat = glm::perspective(vfov, double(kWidth) / double(kHeight), 10.0, 1.0e7);
	glm::dmat4 viewProj = projMat * viewMat;

	Framebuffer fb;
	RasterStats stats;
	for (const auto &pTile : result->tilesToRenderThisFrame) {
		const TileContent &content = pTile->getContent();
		if (!content.isRenderContent()) {
			continue;
		}
		const CesiumGltf::Model &model = content.getRenderContent()->getModel();
		const glm::dmat4 tileTransform = pTile->getTransform();
		model.forEachPrimitiveInScene(
		    -1, [&](const CesiumGltf::Model &m, const CesiumGltf::Node &, const CesiumGltf::Mesh &,
		            const CesiumGltf::MeshPrimitive &prim, const glm::dmat4 &nodeTransform) {
			    rasterizePrimitive(fb, viewProj, tileTransform * nodeTransform, m, prim, stats);
		    });
	}

	const char *outPath = "/tmp/earthview_m0.png";
	stbi_write_png(outPath, kWidth, kHeight, 4, fb.color.data(), kWidth * 4);
	std::printf("Wrote %s\n", outPath);
	std::printf("Stats: %zu primitives (%zu textured), %zu tris, %zu drawn\n", stats.primitives,
	            stats.texturedPrimitives, stats.triangles, stats.trianglesDrawn);

	// Attribution (mandatory display in the real app — printed here)
	std::printf("--- Data attribution ---\n");
	const auto &snapshot = pCredits->getSnapshot();
	for (const auto &credit : snapshot.currentCredits) {
		std::printf("  %s\n", stripHtml(pCredits->getHtml(credit)).c_str());
	}

	const bool ok = stats.trianglesDrawn > 0;
	std::printf(ok ? "M0 PASS\n" : "M0 FAIL: nothing rasterized\n");
	return ok ? 0 : 1;
}
