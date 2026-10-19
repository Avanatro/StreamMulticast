/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "ItemActions.hpp"

#include <algorithm>

namespace smulti::item_actions {

namespace {

Transform bounded(const preview_edit::Item &item, layout::Size canvas, BoundsType type)
{
	Transform t = reset_transform();
	t.crop = item.crop;
	t.scale = item.scale; // unused while bounds apply, kept so removing the bounds later is lossless
	if (canvas.w == 0 || canvas.h == 0)
		return t; // nothing sensible to fit into: position 0, no bounds
	t.bounds_type = type;
	t.bounds = {static_cast<float>(canvas.w), static_cast<float>(canvas.h)};
	return t;
}

struct Extent {
	float min_x, max_x, min_y, max_y;
};

Extent extent(const preview_edit::Item &item)
{
	const auto c = preview_edit::corners(item);
	Extent e{c[0].x, c[0].x, c[0].y, c[0].y};
	for (const auto &p : c) {
		e.min_x = std::min(e.min_x, p.x);
		e.max_x = std::max(e.max_x, p.x);
		e.min_y = std::min(e.min_y, p.y);
		e.max_y = std::max(e.max_y, p.y);
	}
	return e;
}

} // namespace

Transform fit_canvas(const preview_edit::Item &item, layout::Size canvas)
{
	return bounded(item, canvas, BoundsType::ScaleInner);
}

Transform fill_canvas(const preview_edit::Item &item, layout::Size canvas)
{
	return bounded(item, canvas, BoundsType::ScaleOuter);
}

Transform reset_transform()
{
	return Transform{};
}

preview_edit::Vec2 center_horizontally(const preview_edit::Item &item, layout::Size canvas)
{
	const Extent e = extent(item);
	const float dx = static_cast<float>(canvas.w) / 2.0f - (e.min_x + e.max_x) / 2.0f;
	return {item.pos.x + dx, item.pos.y};
}

preview_edit::Vec2 center_vertically(const preview_edit::Item &item, layout::Size canvas)
{
	const Extent e = extent(item);
	const float dy = static_cast<float>(canvas.h) / 2.0f - (e.min_y + e.max_y) / 2.0f;
	return {item.pos.x, item.pos.y + dy};
}

std::string unique_name(const std::string &base, const std::vector<std::string> &existing)
{
	auto taken = [&](const std::string &n) { return std::find(existing.begin(), existing.end(), n) != existing.end(); };
	if (!taken(base))
		return base;
	for (int i = 2;; ++i) {
		const std::string candidate = base + " " + std::to_string(i);
		if (!taken(candidate))
			return candidate;
	}
}

} // namespace smulti::item_actions
