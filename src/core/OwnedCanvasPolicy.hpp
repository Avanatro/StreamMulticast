/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "CanvasMigration.hpp"
#include "CanvasResolve.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace smulti {

enum class OwnedCanvasAction { Create, Reuse, ReuseAndResetVideo };

struct OwnedCanvasDecision {
	OwnedCanvasAction action = OwnedCanvasAction::Create;
	std::string uuid; ///< set for Reuse and ReuseAndResetVideo
};

/**
 * Decide what to do with the plugin-owned canvas given a snapshot list.
 * Matches by exact name kOwnedVerticalCanvasName. Absent -> Create; present
 * with a video mix -> Reuse; present without one -> ReuseAndResetVideo.
 * Pure: no libobs calls.
 */
OwnedCanvasDecision decide_owned_canvas(const std::vector<CanvasInfo> &canvases);

/** Pure subset of obs_video_info; the OBS adapter copies the format fields itself. */
struct VideoGeometry {
	uint32_t base_width = 0, base_height = 0;
	uint32_t output_width = 0, output_height = 0;
	uint32_t fps_num = 0, fps_den = 1;
};

/** Vertical geometry for the owned canvas: base = output = 1080x1920, fps copied from main. */
VideoGeometry derive_vertical_geometry(const VideoGeometry &main);

/**
 * True when the canvas's stored fps differs from the main fps (compared as ratios). libobs obs_reset_video
 * rebuilds non-main canvas mixes from their own stored video info (restore_canvases passes NULL and
 * obs_init_video_mix opens the video output from it), so a main FPS change leaves the owned canvas on the old
 * fps until its video is reset. A zero numerator or denominator on either side is "unknown": false.
 */
bool owned_canvas_fps_stale(const VideoGeometry &canvas, const VideoGeometry &main);

/**
 * Ids of the endpoints bound to removed_uuid. Input pairs are (endpoint id,
 * bound canvas uuid). An empty removed_uuid matches nothing. Input order kept.
 */
std::vector<std::string>
endpoints_to_stop_on_canvas_removed(const std::vector<std::pair<std::string, std::string>> &endpoint_id_and_canvas_uuid,
				    const std::string &removed_uuid);

} // namespace smulti
