/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "CanvasLookup.hpp"

#include <obs.h>

namespace smulti {

namespace {

bool collect_canvas(void *param, obs_canvas_t *canvas)
{
	auto *out = static_cast<std::vector<CanvasInfo> *>(param);

	CanvasInfo info;
	const char *uuid = obs_canvas_get_uuid(canvas);
	const char *name = obs_canvas_get_name(canvas);
	info.uuid      = uuid ? uuid : "";
	info.name      = name ? name : "";
	info.has_video = obs_canvas_has_video(canvas);
	info.is_main   = (obs_canvas_get_flags(canvas) & static_cast<uint32_t>(MAIN)) != 0;
	out->push_back(std::move(info));
	return true; // keep enumerating
}

} // anonymous namespace

std::vector<CanvasInfo> snapshot_canvases()
{
	std::vector<CanvasInfo> result;
	obs_enum_canvases(collect_canvas, &result);
	return result;
}

obs_canvas_t *acquire_canvas(const std::string &uuid)
{
	if (uuid.empty())
		return nullptr;
	return obs_get_canvas_by_uuid(uuid.c_str());
}

} // namespace smulti
