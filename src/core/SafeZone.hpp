/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "LayoutMath.hpp"
#include "PreviewEdit.hpp"

#include <string>

/**
 * Approximate TikTok / YouTube Shorts safe zones for the vertical preview (OBS-free, Qt-free).
 * Preview aid only: the numbers are third-party approximations and never reach the canvas mix.
 */
namespace smulti::safe_zone {

enum class Profile { Off, TikTok, Shorts };

/** Stable config strings: "off" | "tiktok" | "shorts". Unknown text parses to Off. */
const char *profile_to_string(Profile p);
Profile profile_from_string(const std::string &s);

/** Areas in canvas px for one canvas size. Everything is empty for Profile::Off or an empty canvas. */
struct Zone {
	bool active = false;
	layout::Rect unsafe_top;
	layout::Rect unsafe_bottom;
	layout::Rect unsafe_left;  ///< between the top and bottom bands
	layout::Rect unsafe_right; ///< between the top and bottom bands
	bool has_action_column = false;
	layout::Rect action_column; ///< right-hand button column (inside the canvas, may overlap unsafe_right)
	layout::Rect safe;
};

/** The 1080x1920 table scaled linearly (x by width/1080, y by height/1920) to canvas. */
Zone safe_zone_for(Profile profile, layout::Size canvas);

/**
 * Pseudo item for preview_edit::move_position `others`: top-left aligned, scale 1, visible, source =
 * the safe rect, so a dragged box snaps to the safe edges. Inactive zone -> invisible item (ignored).
 */
preview_edit::Item safe_zone_snap_item(const Zone &zone);

} // namespace smulti::safe_zone
