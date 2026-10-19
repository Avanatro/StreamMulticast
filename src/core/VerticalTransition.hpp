/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <cstdint>
#include <functional>
#include <string>

// libobs forward declaration (obs-data only, OBS-free harness safe)
struct obs_data;
typedef struct obs_data obs_data_t;

/**
 * Pure decision for the scene transition of the owned vertical canvas (OBS-free, Qt-free; obs-data only).
 *
 * The vertical canvas' channel 0 holds a plugin-owned private transition whose active child is the vertical
 * scene. When the main transition starts, the adapter (OwnedCanvas) describes it as a TransitionDesc and
 * decide_vertical_transition() says what the vertical transition does: switch instantly, or start a
 * transition of the same type with a copy of the settings and the same duration. A stinger gets its
 * vertical video file when one is configured or found next to the 16:9 file; otherwise the 16:9 file is
 * kept and the plan asks for aspect scaling.
 */
namespace smulti {

/** Save-callback keys (scene collection data), next to the scene link keys. */
inline constexpr const char *kVerticalTransitionModeKey = "streammulticast_vertical_transition_mode";
inline constexpr const char *kVerticalTransitionCustomKey = "streammulticast_vertical_transition_custom";
inline constexpr const char *kVerticalStingerPathKey = "streammulticast_vertical_stinger_path";

/** Transition source ids (libobs / obs-transitions). */
inline constexpr const char *kCutTransitionId = "cut_transition";
inline constexpr const char *kStingerTransitionId = "obs_stinger_transition";

enum class VerticalTransitionMode {
	Follow, ///< same transition as the main canvas (default)
	Cut,    ///< always instant
	Custom, ///< a named frontend transition
};

/** "follow" | "cut" | "custom". */
const char *vertical_transition_mode_name(VerticalTransitionMode mode);

/** Inverse of vertical_transition_mode_name; anything unknown or empty is Follow. */
VerticalTransitionMode parse_vertical_transition_mode(const std::string &name);

struct VerticalTransitionSettings {
	VerticalTransitionMode mode = VerticalTransitionMode::Follow;
	std::string custom_name;  ///< frontend transition name used in Custom mode
	std::string stinger_path; ///< "" = auto-detect the vertical stinger file
};

/** Write mode, custom name and stinger path into collection data. */
void vertical_transition_save(obs_data_t *data, const VerticalTransitionSettings &settings);

/** Read them back; missing keys yield the defaults (follow, no custom name, auto stinger path). */
VerticalTransitionSettings vertical_transition_load(obs_data_t *data);

/** One transition as the adapter sees it. `settings` is borrowed (may be null). */
struct TransitionDesc {
	std::string id;
	std::string name;
	obs_data_t *settings = nullptr;
	uint32_t duration_ms = 0;
};

/** What the vertical transition does. */
struct VerticalTransitionPlan {
	bool instant = true;       ///< obs_transition_set (no animation)
	std::string id;            ///< transition type id when animated
	std::string settings_json; ///< settings for the vertical transition (obs_data json) when animated
	uint32_t duration_ms = 0;
	bool scale_aspect = false; ///< 16:9 stinger kept: OBS_TRANSITION_SCALE_ASPECT
};

using FileExists = std::function<bool(const std::string &path)>;

/**
 * The vertical variant of a stinger video: `configured` when it is set and exists; otherwise the first
 * existing of "<dir>/<stem>-vertical<.ext>" and "<dir>/stinger-vertical<.ext>"; otherwise "" (none found).
 * Both '/' and '\\' separate directories.
 */
std::string vertical_stinger_file(const std::string &path, const std::string &configured, const FileExists &exists);

/**
 * Cut mode, an empty id or a cut transition -> instant. Otherwise the transition of `main` (Follow) or of
 * `custom` (Custom; a null custom falls back to `main`) with a copy of its settings and its duration.
 * Stinger: "path" becomes vertical_stinger_file(path, settings.stinger_path) when one exists, else the
 * path is kept and scale_aspect is set; with "track_matte_enabled" the "track_matte_path" gets the
 * "<stem>-vertical" file when it exists. "transition_point" and every other key are copied unchanged.
 */
VerticalTransitionPlan decide_vertical_transition(const VerticalTransitionSettings &settings,
						  const TransitionDesc &main, const TransitionDesc *custom,
						  const FileExists &exists);

/**
 * Duration of the main transition that just started: the destination scene's override duration when the
 * started transition is that scene's override transition (frontend per-scene override), else the frontend
 * duration. Quick transition durations are not exposed by the frontend API, so they use the frontend
 * duration too. A non-positive override duration yields the frontend duration.
 */
uint32_t main_transition_duration_ms(const std::string &started_name, const std::string &override_name,
				     int64_t override_duration_ms, int frontend_duration_ms);

} // namespace smulti
