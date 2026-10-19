/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "CanvasMigration.hpp"

namespace smulti {

bool migrate_legacy_orientation(Endpoint &ep)
{
	if (ep.orientation != OutputOrientation::Vertical1080x1920Letterbox &&
	    ep.orientation != OutputOrientation::Vertical1080x1920CenterCrop)
		return false;

	ep.canvas_name = kOwnedVerticalCanvasName;
	ep.canvas_uuid.clear();
	ep.orientation = OutputOrientation::SourceMatch;
	return true;
}

ConfigMigrationResult migrate_config(int from_version, int to_version, std::vector<Endpoint> &endpoints)
{
	ConfigMigrationResult res;
	if (from_version >= to_version)
		return res;

	res.needs_save = true;

	/* v1 -> v2: framing modes 1/2 were removed in favour of a canvas reference. */
	if (from_version < 2) {
		for (auto &ep : endpoints) {
			const int old_mode = static_cast<int>(ep.orientation);
			if (migrate_legacy_orientation(ep))
				res.migrated.push_back({ep.name, old_mode});
		}
	}
	return res;
}

} // namespace smulti
