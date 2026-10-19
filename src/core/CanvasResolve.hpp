/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <string>
#include <vector>

namespace smulti {

/** Snapshot of one OBS canvas, taken on a thread that may call libobs. */
struct CanvasInfo {
	std::string uuid;
	std::string name;
	bool has_video = false;
	bool is_main = false;
};

struct CanvasResolution {
	enum class Kind { Main, Canvas, Refused } kind = Kind::Main;
	std::string uuid;   // set for Canvas
	std::string name;   // resolved canvas name, for logs
	std::string reason; // set for Refused, human readable, never contains a key
};

/**
 * Resolve an endpoint's canvas reference against a snapshot list.
 * Both refs empty -> Main. The uuid is tried first, then the name. A ref that
 * points at the canvas flagged is_main resolves to Main. A hit without a video
 * mix and a complete miss are Refused with a reason. Pure: no libobs calls.
 */
CanvasResolution resolve_canvas(const std::string &ref_uuid, const std::string &ref_name,
				const std::vector<CanvasInfo> &canvases);

} // namespace smulti
