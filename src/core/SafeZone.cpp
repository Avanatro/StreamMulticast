/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "SafeZone.hpp"

#include <algorithm>
#include <cmath>

namespace smulti::safe_zone {

namespace {

/* Reference frame of the table below. */
constexpr int kRefW = 1080;
constexpr int kRefH = 1920;

struct Row {
	int top, bottom, left, right;               ///< unsafe margins in reference px
	bool has_action_column;
	int col_x, col_y, col_w, col_h;             ///< right-hand button column in reference px
};

/*
 * ONE table, one source comment per row. All numbers are APPROXIMATIONS: neither platform publishes
 * margins for LIVE or organic video, the figures come from the ad templates and third-party tools, and
 * the apps move their UI between versions. Adjust here only; nothing else holds a zone number.
 *
 * TikTok (approximation): top 240, bottom 660, left/right 120 from TikTok's In-Feed Ads template (720x1280
 * frame, scaled to 1080x1920 by socialync.io, https://socialync.io/tools/safe-zones/tiktok). The button column
 * (x 900..1080, y 840..1260) is socialync.io's own measurement (180 wide from y 840), not confirmed by
 * TikTok; its height is a planner estimate. Other tools publish 160..254 top and 480..707 bottom
 * (adkit.so, revid.ai, poster.ly); this set is the most conservative that still leaves a usable centre.
 *
 * Shorts (approximation): top 288, bottom 672, left 48, right 192 from Google Ads Help "safe zones for
 * vertical video ads" (https://support.google.com/google-ads/answer/9128498, 15% / 35% / 4% / 18% of the
 * frame), as quoted by https://www.socialync.io/tools/safe-zones/youtube-shorts and https://www.revid.ai/youtube-shorts-safe-zone.
 * Published for ads; organic Shorts may differ. No button column is given.
 */
constexpr Row kTikTok = {240, 660, 120, 120, true, 900, 840, 180, 420};
constexpr Row kShorts = {288, 672, 48, 192, false, 0, 0, 0, 0};

int sx(int v, layout::Size c) { return static_cast<int>(std::lround(static_cast<double>(v) * c.w / kRefW)); }
int sy(int v, layout::Size c) { return static_cast<int>(std::lround(static_cast<double>(v) * c.h / kRefH)); }

} // namespace

const char *profile_to_string(Profile p)
{
	switch (p) {
	case Profile::TikTok:
		return "tiktok";
	case Profile::Shorts:
		return "shorts";
	default:
		return "off";
	}
}

Profile profile_from_string(const std::string &s)
{
	if (s == "tiktok")
		return Profile::TikTok;
	if (s == "shorts")
		return Profile::Shorts;
	return Profile::Off;
}

Zone safe_zone_for(Profile profile, layout::Size canvas)
{
	Zone z;
	const Row *row = profile == Profile::TikTok ? &kTikTok : profile == Profile::Shorts ? &kShorts : nullptr;
	if (!row || canvas.w == 0 || canvas.h == 0)
		return z;

	const int w = static_cast<int>(canvas.w);
	const int h = static_cast<int>(canvas.h);
	const int top = std::min(sy(row->top, canvas), h);
	const int bottom = std::min(sy(row->bottom, canvas), h - top);
	const int left = std::min(sx(row->left, canvas), w);
	const int right = std::min(sx(row->right, canvas), w - left);
	const int mid_h = h - top - bottom;

	z.active = true;
	z.unsafe_top = {0, 0, w, top};
	z.unsafe_bottom = {0, h - bottom, w, bottom};
	z.unsafe_left = {0, top, left, mid_h};
	z.unsafe_right = {w - right, top, right, mid_h};
	z.safe = {left, top, w - left - right, mid_h};
	if (row->has_action_column) {
		z.has_action_column = true;
		const int x = sx(row->col_x, canvas);
		z.action_column = {x, sy(row->col_y, canvas), std::max(0, w - x), sy(row->col_h, canvas)};
	}
	return z;
}

preview_edit::Item safe_zone_snap_item(const Zone &zone)
{
	preview_edit::Item it;
	it.id = -1; // never a real scene item id
	it.visible = zone.active && zone.safe.w > 0 && zone.safe.h > 0;
	it.locked = true;
	it.pos = {static_cast<float>(zone.safe.x), static_cast<float>(zone.safe.y)};
	it.source = {static_cast<uint32_t>(std::max(0, zone.safe.w)), static_cast<uint32_t>(std::max(0, zone.safe.h))};
	return it;
}

} // namespace smulti::safe_zone
