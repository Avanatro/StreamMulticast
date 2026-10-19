/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalTransition.hpp"

#include <obs-data.h>

namespace smulti {

namespace {

constexpr const char *kModeFollow = "follow";
constexpr const char *kModeCut = "cut";
constexpr const char *kModeCustom = "custom";

/** Splits "<dir><sep>" + "<stem>" + "<.ext>" (ext includes the dot, may be empty). */
struct PathParts {
	std::string dir; ///< including the trailing separator, may be empty
	std::string stem;
	std::string ext;
};

PathParts split_path(const std::string &path)
{
	PathParts out;
	const size_t sep = path.find_last_of("/\\");
	const size_t name_start = sep == std::string::npos ? 0 : sep + 1;
	out.dir = path.substr(0, name_start);
	const std::string name = path.substr(name_start);
	const size_t dot = name.find_last_of('.');
	if (dot == std::string::npos || dot == 0) {
		out.stem = name;
	} else {
		out.stem = name.substr(0, dot);
		out.ext = name.substr(dot);
	}
	return out;
}

/** "<dir>/<stem>-vertical<.ext>" when it exists, else "". */
std::string suffixed_vertical_file(const std::string &path, const FileExists &exists)
{
	if (path.empty() || !exists)
		return {};
	const PathParts p = split_path(path);
	if (p.stem.empty())
		return {};
	const std::string candidate = p.dir + p.stem + "-vertical" + p.ext;
	return exists(candidate) ? candidate : std::string();
}

std::string get_string(obs_data_t *data, const char *key)
{
	const char *v = data ? obs_data_get_string(data, key) : nullptr;
	return v ? v : "";
}

} // namespace

const char *vertical_transition_mode_name(VerticalTransitionMode mode)
{
	switch (mode) {
	case VerticalTransitionMode::Cut:
		return kModeCut;
	case VerticalTransitionMode::Custom:
		return kModeCustom;
	case VerticalTransitionMode::Follow:
	default:
		return kModeFollow;
	}
}

VerticalTransitionMode parse_vertical_transition_mode(const std::string &name)
{
	if (name == kModeCut)
		return VerticalTransitionMode::Cut;
	if (name == kModeCustom)
		return VerticalTransitionMode::Custom;
	return VerticalTransitionMode::Follow;
}

void vertical_transition_save(obs_data_t *data, const VerticalTransitionSettings &settings)
{
	if (!data)
		return;
	obs_data_set_string(data, kVerticalTransitionModeKey, vertical_transition_mode_name(settings.mode));
	obs_data_set_string(data, kVerticalTransitionCustomKey, settings.custom_name.c_str());
	obs_data_set_string(data, kVerticalStingerPathKey, settings.stinger_path.c_str());
}

VerticalTransitionSettings vertical_transition_load(obs_data_t *data)
{
	VerticalTransitionSettings out;
	if (!data)
		return out;
	out.mode = parse_vertical_transition_mode(get_string(data, kVerticalTransitionModeKey));
	out.custom_name = get_string(data, kVerticalTransitionCustomKey);
	out.stinger_path = get_string(data, kVerticalStingerPathKey);
	return out;
}

std::string vertical_stinger_file(const std::string &path, const std::string &configured, const FileExists &exists)
{
	if (!exists)
		return {};
	if (!configured.empty() && exists(configured))
		return configured;
	const std::string suffixed = suffixed_vertical_file(path, exists);
	if (!suffixed.empty())
		return suffixed;
	if (path.empty())
		return {};
	const PathParts p = split_path(path);
	const std::string generic = p.dir + "stinger-vertical" + p.ext;
	return exists(generic) ? generic : std::string();
}

VerticalTransitionPlan decide_vertical_transition(const VerticalTransitionSettings &settings,
						  const TransitionDesc &main, const TransitionDesc *custom,
						  const FileExists &exists)
{
	VerticalTransitionPlan plan; // instant by default
	if (settings.mode == VerticalTransitionMode::Cut)
		return plan;

	const TransitionDesc &source =
		(settings.mode == VerticalTransitionMode::Custom && custom) ? *custom : main;
	if (source.id.empty() || source.id == kCutTransitionId)
		return plan;

	obs_data_t *copy = obs_data_create();
	if (source.settings)
		obs_data_apply(copy, source.settings);

	if (source.id == kStingerTransitionId) {
		const std::string path = get_string(copy, "path");
		const std::string vertical = vertical_stinger_file(path, settings.stinger_path, exists);
		if (!vertical.empty())
			obs_data_set_string(copy, "path", vertical.c_str());
		else
			plan.scale_aspect = true; // keep the 16:9 file
		if (obs_data_get_bool(copy, "track_matte_enabled")) {
			const std::string matte = suffixed_vertical_file(get_string(copy, "track_matte_path"), exists);
			if (!matte.empty())
				obs_data_set_string(copy, "track_matte_path", matte.c_str());
		}
	}

	const char *json = obs_data_get_json(copy);
	plan.settings_json = json ? json : "{}";
	obs_data_release(copy);

	plan.instant = false;
	plan.id = source.id;
	plan.duration_ms = source.duration_ms;
	return plan;
}

uint32_t main_transition_duration_ms(const std::string &started_name, const std::string &override_name,
				     int64_t override_duration_ms, int frontend_duration_ms)
{
	if (!override_name.empty() && override_name == started_name && override_duration_ms > 0)
		return static_cast<uint32_t>(override_duration_ms);
	return frontend_duration_ms > 0 ? static_cast<uint32_t>(frontend_duration_ms) : 0u;
}

} // namespace smulti
