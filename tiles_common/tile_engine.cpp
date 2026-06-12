// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#include "tile_engine.h"
#include "geo_math.h"

#include <Cesium3DTilesContent/registerAllTileContentTypes.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/ViewState.h>
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/IAssetAccessor.h>
#include <CesiumAsync/IAssetRequest.h>
#include <CesiumAsync/ITaskProcessor.h>
#include <CesiumCurl/CurlAssetAccessor.h>
#include <CesiumGeospatial/Ellipsoid.h>
#include <CesiumUtility/CreditSystem.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <thread>

namespace {

// SSE knob: cesium default 16, tightened ~0.9× so the center-eye selection
// is conservative for both eyes (PRD §6.2).
constexpr double kMaxScreenSpaceError = 16.0 * 0.9;
// cesium's built-in per-updateView budget for prepareInMainThread work (ms) —
// this is what amortizes our GPU uploads across frames; do not hand-roll.
constexpr double kMainThreadLoadingTimeLimitMs = 5.0;
// Attribution recount cadence (frames). HUD refreshes ~2 Hz anyway.
constexpr int kAttributionEveryNFrames = 30;

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
// request for Google P3DT (M0 fact 3), so a bare CurlAssetAccessor is
// sufficient. This decorator is a belt-and-suspenders safeguard only.
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
stripHtml(const std::string &html)
{
	static const std::regex tags("<[^>]*>");
	return std::regex_replace(html, tags, "");
}

} // namespace

std::string
earthviewGetApiKey()
{
	if (const char *env = std::getenv("GOOGLE_MAPS_API_KEY"); env && *env) {
		return env;
	}
	for (const char *path : {"earthview.ini", "../earthview.ini", "../../earthview.ini"}) {
		std::ifstream f(path);
		std::string line;
		while (std::getline(f, line)) {
			if (line.rfind("key=", 0) == 0 && line.size() > 4) {
				std::string key = line.substr(4);
				while (!key.empty() &&
				       (key.back() == '\r' || key.back() == ' ' || key.back() == '\n')) {
					key.pop_back();
				}
				return key;
			}
		}
	}
	return {};
}

struct TileEngine::Impl
{
	CesiumAsync::AsyncSystem asyncSystem{std::make_shared<ThreadTaskProcessor>()};
	std::shared_ptr<CesiumUtility::CreditSystem> credits;
	std::unique_ptr<Cesium3DTilesSelection::Tileset> tileset;
	const Cesium3DTilesSelection::ViewUpdateResult *lastResult = nullptr;
};

TileEngine::TileEngine() : impl_(std::make_unique<Impl>()) {}

TileEngine::~TileEngine() = default;

bool
TileEngine::init(Cesium3DTilesSelection::IPrepareRendererResources *prepare)
{
	using namespace Cesium3DTilesSelection;

	key_ = earthviewGetApiKey();
	if (key_.empty()) {
		return false; // app stays up — first-run card (PRD §7.4)
	}

	spdlog::set_level(spdlog::level::critical); // mute per-request parse-warning spam

	// REQUIRED (M0 fact 1): without this the GltfConverters registry is empty
	// and every glb tile silently fails into the JSON tileset parser.
	Cesium3DTilesContent::registerAllTileContentTypes();

	auto pAccessor = std::make_shared<KeyInjectingAccessor>(
	    std::make_shared<CesiumCurl::CurlAssetAccessor>(), key_);
	impl_->credits = std::make_shared<CesiumUtility::CreditSystem>();

	// Non-owning: TileRenderer is owned by the app and outlives the engine.
	std::shared_ptr<IPrepareRendererResources> pPrepare(prepare,
	                                                    [](IPrepareRendererResources *) {});

	TilesetExternals externals{pAccessor, pPrepare, impl_->asyncSystem, impl_->credits};

	TilesetOptions options;
	options.maximumScreenSpaceError = kMaxScreenSpaceError;
	options.mainThreadLoadingTimeLimit = kMainThreadLoadingTimeLimitMs;
	options.showCreditsOnScreen = true;
	// Never render holes during REPLACE refinement: keep the parent up until
	// ALL children are renderable. Default(false) kicks the parent while
	// children stream in — visible as missing ground where detail demand is
	// highest (close range / pointing down).
	options.forbidHoles = true;

	const std::string url = "https://tile.googleapis.com/v1/3dtiles/root.json?key=" + key_;
	impl_->tileset = std::make_unique<Tileset>(externals, url, options);
	return true;
}

void
TileEngine::shutdown()
{
	impl_->lastResult = nullptr;
	impl_->tileset.reset();
	lastRenderTiles_ = 0;
}

const Cesium3DTilesSelection::ViewUpdateResult &
TileEngine::update(const geo::GeoCamera &cam, double viewW, double viewH, double hfovRad,
                   double vfovRad)
{
	using namespace Cesium3DTilesSelection;

	// ViewState assumes an orthonormal dir/up pair; our geodetic up is not
	// perpendicular to a tilted view dir, which skews the culling frustum
	// (amplified at wide FOV). Re-orthogonalize.
	glm::dvec3 upOrtho = cam.up - cam.dir * glm::dot(cam.up, cam.dir);
	double ul = glm::length(upOrtho);
	upOrtho = ul > 1e-12 ? upOrtho / ul : glm::dvec3(0.0, 0.0, 1.0);

	ViewState view(cam.pos, cam.dir, upOrtho, glm::dvec2(viewW, viewH), hfovRad, vfovRad,
	               CesiumGeospatial::Ellipsoid::WGS84);
	const ViewUpdateResult &result = impl_->tileset->updateView({view});
	impl_->asyncSystem.dispatchMainThreadTasks();
	impl_->lastResult = &result;

	lastRenderTiles_ = 0;
	for (const auto &pTile : result.tilesToRenderThisFrame) {
		if (pTile->getContent().isRenderContent()) {
			++lastRenderTiles_;
		}
	}
	++frameCounter_;
	return result;
}

const AttributionInfo &
TileEngine::attribution()
{
	if (!impl_->lastResult || !impl_->credits ||
	    (frameCounter_ % kAttributionEveryNFrames) != 0) {
		return attribution_;
	}

	const auto &result = *impl_->lastResult;

	// Frequency count across visible render tiles: each tile carries its
	// credit list; combined copyright strings split on ';' (Google packs
	// several providers into one credit).
	std::map<std::string, int> counts;
	for (const auto &pTile : result.tilesToRenderThisFrame) {
		const auto &content = pTile->getContent();
		if (!content.isRenderContent()) {
			continue;
		}
		for (const auto &credit : content.getRenderContent()->getCredits()) {
			std::string text = stripHtml(impl_->credits->getHtml(credit));
			size_t start = 0;
			while (start <= text.size()) {
				size_t semi = text.find(';', start);
				std::string part = text.substr(
				    start, semi == std::string::npos ? std::string::npos : semi - start);
				// trim
				size_t b = part.find_first_not_of(" \t");
				size_t e = part.find_last_not_of(" \t");
				if (b != std::string::npos) {
					counts[part.substr(b, e - b + 1)]++;
				}
				if (semi == std::string::npos) {
					break;
				}
				start = semi + 1;
			}
		}
	}

	std::vector<std::pair<std::string, int>> sorted(counts.begin(), counts.end());
	std::sort(sorted.begin(), sorted.end(),
	          [](const auto &a, const auto &b) { return a.second > b.second; });

	attribution_.credits.clear();
	for (const auto &[text, n] : sorted) {
		attribution_.credits.push_back(text);
	}
	attribution_.tilesInFlight = (int)(result.workerThreadTileLoadQueueLength +
	                                   result.mainThreadTileLoadQueueLength);
	attribution_.renderTiles = lastRenderTiles_;
	return attribution_;
}
