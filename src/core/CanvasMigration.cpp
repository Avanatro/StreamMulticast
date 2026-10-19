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

} // namespace smulti
