/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

// libobs forward declaration (obs-data only, OBS-free harness safe)
struct obs_data;
typedef struct obs_data obs_data_t;

namespace smulti {

/** Save-callback keys (scene collection data). */
inline constexpr const char *kSceneLinkMapKey = "streammulticast_vertical_links";
inline constexpr const char *kSceneLinkEnabledKey = "streammulticast_vertical_link_enabled";

/**
 * Link between the main program scene and the scene on channel 0 of the owned
 * vertical canvas. `explicit_map` maps main scene name -> vertical scene name;
 * "" means "keep the current vertical scene". No entry means "auto".
 * How the switch animates is VerticalTransition's job (the old "wait for stinger transition point"
 * option and its key streammulticast_vertical_link_wait_stinger are gone; a saved key is ignored).
 */
struct SceneLinkSettings {
	bool enabled = true;
	std::map<std::string, std::string> explicit_map;
};

/**
 * Pure resolution. Returns the vertical scene to switch to, or nullopt for
 * "keep current" (link disabled, explicit keep, or nothing matches).
 *
 * Explicit entry present: "" -> keep; names an existing vertical scene -> it;
 * names a missing scene -> falls through to the default rule.
 * Default rule: vertical "V <main>", else vertical "<main>", else keep.
 */
std::optional<std::string> resolve_linked_scene(const std::string &main_scene, const SceneLinkSettings &settings,
						const std::vector<std::string> &vertical_scenes);

/** A scene was renamed: re-key / re-target explicit entries. Returns true when anything changed. */
bool rename_scene_in_links(SceneLinkSettings &settings, const std::string &old_name, const std::string &new_name);

/** Write enabled flag and map into collection data. */
void scene_link_save(obs_data_t *data, const SceneLinkSettings &settings);

/** Read settings from collection data; missing keys yield the defaults (enabled, empty map). */
SceneLinkSettings scene_link_load(obs_data_t *data);

} // namespace smulti
