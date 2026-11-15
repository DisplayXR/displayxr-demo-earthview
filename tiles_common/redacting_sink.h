// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// redacting_sink — an spdlog sink that masks the Map Tiles API key before a
// line reaches the real sink. cesium-native logs failed tile loads with the
// full request URL ("Failed to load <url>", "Received status code N for tile
// content <url>"), and every Google tile URL carries `key=<api key>`, so any
// leg that routes cesium's logger somewhere visible (Android: logcat) must
// wrap that sink in this one. Masking is earthviewRedactKey().

#pragma once

#include "tile_engine.h"

#include <spdlog/details/log_msg.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace earthview {

template <typename Mutex>
class redacting_sink final : public spdlog::sinks::base_sink<Mutex>
{
public:
	explicit redacting_sink(std::shared_ptr<spdlog::sinks::sink> inner) : inner_(std::move(inner)) {}

protected:
	void
	sink_it_(const spdlog::details::log_msg &msg) override
	{
		const std::string text(msg.payload.data(), msg.payload.size());
		const std::string masked = earthviewRedactKey(text);
		if (masked == text) {
			inner_->log(msg);
			return;
		}
		spdlog::details::log_msg copy = msg;
		copy.payload = spdlog::string_view_t(masked.data(), masked.size());
		inner_->log(copy);
	}

	void
	flush_() override
	{
		inner_->flush();
	}

private:
	std::shared_ptr<spdlog::sinks::sink> inner_;
};

using redacting_sink_mt = redacting_sink<std::mutex>;
using redacting_sink_st = redacting_sink<spdlog::details::null_mutex>;

} // namespace earthview
