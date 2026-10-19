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
 * vertical video file when one is configured or found next to the 16:9 file; otherwise (OBS would stretch
 * the 16:9 video to 9:16) the plan is a fade over the stinger's transition point (owner ruling R2).
 */
namespace smulti {

/** Save-callback keys (scene collection data), next to the scene link keys. */
inline constexpr const char *kVerticalTransitionModeKey = "streammulticast_vertical_transition_mode";
inline constexpr const char *kVerticalTransitionCustomKey = "streammulticast_vertical_transition_custom";
inline constexpr const char *kVerticalStingerPathKey = "streammulticast_vertical_stinger_path";
inline constexpr const char *kVerticalStingerSourceKey = "streammulticast_vertical_stinger_source";

/** Transition source ids (libobs / obs-transitions). */
inline constexpr const char *kCutTransitionId = "cut_transition";
inline constexpr const char *kFadeTransitionId = "fade_transition";
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
	std::string stinger_path;   ///< "" = auto-detect the vertical stinger file
	std::string stinger_source; ///< the 16:9 stinger file `stinger_path` was chosen for (applies to it only)
};

/** Write mode, custom name, stinger path and its source file into collection data. */
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
	bool fade_fallback = false; ///< stinger without a vertical file: fade (id fade_transition, duration set)
};

using FileExists = std::function<bool(const std::string &path)>;

/**
 * The vertical variant of the stinger video `path`: `configured` when it is set, was chosen for this `path`
 * (`configured_for` == `path`) and exists; otherwise the first existing of "<dir>/<stem>-vertical<.ext>"
 * and "<dir>/stinger-vertical<.ext>" (same extension as `path`); otherwise "" (none found).
 * Both '/' and '\\' separate directories.
 */
std::string vertical_stinger_file(const std::string &path, const std::string &configured,
				  const std::string &configured_for, const FileExists &exists);

/**
 * Cut mode, an empty id or a cut transition -> instant. Otherwise the transition of `main` (Follow) or of
 * `custom` (Custom; a null custom falls back to `main`) with a copy of its settings and its duration.
 * Stinger: "path" becomes vertical_stinger_file(path, settings.stinger_path, settings.stinger_source);
 * "audio_monitoring" is forced to 0 (the main stinger already monitors); with "track_matte_enabled" and
 * the separate-file layout (track_matte_layout 2) "track_matte_path" needs its "<stem>-vertical" file too.
 * No vertical video (or no vertical separate matte) -> fade_fallback: fade_transition, settings "{}",
 * duration = the transition point when it is in ms (tp_type 0, > 0), else the transition's duration.
 * "transition_point" and every other key are copied unchanged.
 */
VerticalTransitionPlan decide_vertical_transition(const VerticalTransitionSettings &settings,
						  const TransitionDesc &main, const TransitionDesc *custom,
						  const FileExists &exists);

/**
 * Duration of the main transition that just started: the destination scene's override duration when the
 * started transition is that scene's override transition (frontend per-scene override), else the frontend
 * duration. Quick transition durations are not exposed by the frontend API, so they use the frontend
 * duration too. A non-positive override duration yields the frontend duration. Known gap: the frontend
 * skips the override while one is already installed or for a quick transition of the same name; this
 * rule cannot see that and uses the override duration then (only matters for non-fixed-duration types).
 */
uint32_t main_transition_duration_ms(const std::string &started_name, const std::string &override_name,
				     int64_t override_duration_ms, int frontend_duration_ms);

} // namespace smulti
