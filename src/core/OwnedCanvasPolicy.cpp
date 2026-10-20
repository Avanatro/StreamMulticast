/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "OwnedCanvasPolicy.hpp"

namespace smulti {

OwnedCanvasDecision decide_owned_canvas(const std::vector<CanvasInfo> &canvases)
{
	for (const CanvasInfo &c : canvases) {
		if (c.name != kOwnedVerticalCanvasName)
			continue;
		OwnedCanvasDecision d;
		d.action = c.has_video ? OwnedCanvasAction::Reuse : OwnedCanvasAction::ReuseAndResetVideo;
		d.uuid = c.uuid;
		return d;
	}
	return {};
}

VideoGeometry derive_vertical_geometry(const VideoGeometry &main)
{
	VideoGeometry v;
	v.base_width = v.output_width = 1080;
	v.base_height = v.output_height = 1920;
	v.fps_num = main.fps_num;
	v.fps_den = main.fps_den;
	return v;
}

std::vector<std::string>
endpoints_to_stop_on_canvas_removed(const std::vector<std::pair<std::string, std::string>> &endpoint_id_and_canvas_uuid,
				    const std::string &removed_uuid)
{
	std::vector<std::string> ids;
	if (removed_uuid.empty())
		return ids;
	for (const auto &p : endpoint_id_and_canvas_uuid)
		if (p.second == removed_uuid)
			ids.push_back(p.first);
	return ids;
}

bool owned_canvas_fps_stale(const VideoGeometry &canvas, const VideoGeometry &main)
{
	if (!canvas.fps_num || !canvas.fps_den || !main.fps_num || !main.fps_den)
		return false;
	return uint64_t(canvas.fps_num) * main.fps_den != uint64_t(main.fps_num) * canvas.fps_den;
}

} // namespace smulti
