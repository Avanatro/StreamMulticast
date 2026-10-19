/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "CanvasResolve.hpp"

namespace smulti {

CanvasResolution resolve_canvas(const std::string &ref_uuid, const std::string &ref_name,
				const std::vector<CanvasInfo> &canvases)
{
	CanvasResolution res;
	if (ref_uuid.empty() && ref_name.empty())
		return res; // Kind::Main by default

	const CanvasInfo *hit = nullptr;
	if (!ref_uuid.empty()) {
		for (const CanvasInfo &c : canvases) {
			if (c.uuid == ref_uuid) {
				hit = &c;
				break;
			}
		}
	}
	if (!hit && !ref_name.empty()) {
		for (const CanvasInfo &c : canvases) {
			if (c.name == ref_name) {
				hit = &c;
				break;
			}
		}
	}

	if (!hit) {
		res.kind = CanvasResolution::Kind::Refused;
		res.reason = "canvas '" + (ref_name.empty() ? ref_uuid : ref_name) + "' not found";
		return res;
	}
	if (hit->is_main)
		return res;
	if (!hit->has_video) {
		res.kind = CanvasResolution::Kind::Refused;
		res.reason = "canvas '" + (hit->name.empty() ? hit->uuid : hit->name) + "' has no video mix";
		return res;
	}

	res.kind = CanvasResolution::Kind::Canvas;
	res.uuid = hit->uuid;
	res.name = hit->name;
	return res;
}

} // namespace smulti
