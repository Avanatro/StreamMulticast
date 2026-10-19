/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "CanvasSceneNames.hpp"

#include <obs.h>

namespace smulti {

namespace {

bool collect_scene_name(void *param, obs_source_t *scene)
{
	const char *name = obs_source_get_name(scene);
	if (name)
		static_cast<std::vector<std::string> *>(param)->emplace_back(name);
	return true;
}

} // namespace

std::vector<std::string> canvas_scene_names(obs_canvas_t *canvas)
{
	std::vector<std::string> out;
	obs_canvas_enum_scenes(canvas, collect_scene_name, &out);
	return out;
}

bool canvas_has_name(obs_canvas_t *canvas, const std::string &name)
{
	obs_source_t *src = obs_canvas_get_source_by_name(canvas, name.c_str());
	if (!src)
		return false;
	obs_source_release(src);
	return true;
}

std::string unique_name_in(obs_canvas_t *canvas, const std::string &base, const char *open, const char *close)
{
	if (!canvas_has_name(canvas, base))
		return base;
	for (int i = 2; i < 10000; ++i) {
		std::string candidate = base + open + std::to_string(i) + close;
		if (!canvas_has_name(canvas, candidate))
			return candidate;
	}
	return base + open + "new" + close;
}

} // namespace smulti
