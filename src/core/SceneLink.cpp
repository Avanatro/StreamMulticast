/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "SceneLink.hpp"

#include <obs-data.h>

#include <algorithm>

namespace smulti {

namespace {

bool contains(const std::vector<std::string> &names, const std::string &name)
{
	return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

std::optional<std::string> resolve_linked_scene(const std::string &main_scene, const SceneLinkSettings &settings,
						const std::vector<std::string> &vertical_scenes)
{
	if (!settings.enabled)
		return std::nullopt;

	const auto it = settings.explicit_map.find(main_scene);
	if (it != settings.explicit_map.end()) {
		if (it->second.empty())
			return std::nullopt; // explicit "keep current"
		if (contains(vertical_scenes, it->second))
			return it->second;
		/* entry names a scene that no longer exists: fall through to the default rule */
	}

	const std::string prefixed = "V " + main_scene;
	if (contains(vertical_scenes, prefixed))
		return prefixed;
	if (contains(vertical_scenes, main_scene))
		return main_scene;
	return std::nullopt;
}

bool scene_changed_needs_catch_up(const std::optional<std::string> &target, const std::string &active_or_destination)
{
	return target.has_value() && *target != active_or_destination;
}

bool rename_scene_in_links(SceneLinkSettings &settings, const std::string &old_name, const std::string &new_name)
{
	if (old_name == new_name)
		return false;
	bool changed = false;

	/* values first (vertical scene renamed), then the key (main scene renamed) */
	for (auto &entry : settings.explicit_map) {
		if (entry.second == old_name) {
			entry.second = new_name;
			changed = true;
		}
	}
	const auto node = settings.explicit_map.find(old_name);
	if (node != settings.explicit_map.end()) {
		std::string value = node->second;
		settings.explicit_map.erase(node);
		settings.explicit_map[new_name] = std::move(value);
		changed = true;
	}
	return changed;
}

void scene_link_save(obs_data_t *data, const SceneLinkSettings &settings)
{
	if (!data)
		return;
	obs_data_set_bool(data, kSceneLinkEnabledKey, settings.enabled);

	obs_data_t *map = obs_data_create();
	for (const auto &entry : settings.explicit_map)
		obs_data_set_string(map, entry.first.c_str(), entry.second.c_str());
	obs_data_set_obj(data, kSceneLinkMapKey, map);
	obs_data_release(map);
}

SceneLinkSettings scene_link_load(obs_data_t *data)
{
	SceneLinkSettings out;
	if (!data)
		return out;

	if (obs_data_has_user_value(data, kSceneLinkEnabledKey))
		out.enabled = obs_data_get_bool(data, kSceneLinkEnabledKey);

	obs_data_t *map = obs_data_get_obj(data, kSceneLinkMapKey);
	if (map) {
		for (obs_data_item_t *item = obs_data_first(map); item; obs_data_item_next(&item)) {
			const char *key = obs_data_item_get_name(item);
			const char *value = obs_data_item_get_string(item);
			if (key && *key)
				out.explicit_map[key] = value ? value : "";
		}
		obs_data_release(map);
	}
	return out;
}

} // namespace smulti
