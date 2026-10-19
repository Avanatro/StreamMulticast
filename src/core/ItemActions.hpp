/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "PreviewEdit.hpp"

#include <string>
#include <vector>

/**
 * Pure helpers for the item actions of the vertical dock (OBS-free, Qt-free): the "Transform" presets
 * and unique source names. Geometry types are shared with PreviewEdit / LayoutMath.
 */
namespace smulti::item_actions {

enum class BoundsType { None, ScaleInner, ScaleOuter };

/** What a preset writes (all in canvas px). The adapter maps it onto obs_transform_info + crop. */
struct Transform {
	preview_edit::Vec2 pos;
	float rot = 0;
	preview_edit::Vec2 scale{1, 1};
	uint32_t alignment = preview_edit::AlignLeft | preview_edit::AlignTop;
	BoundsType bounds_type = BoundsType::None;
	preview_edit::Vec2 bounds;
	uint32_t bounds_alignment = 0; ///< 0 = centre, so the source is centred inside the bounds
	layout::Crop crop;
};

/** Bounds SCALE_INNER of the canvas size at (0,0), centred inside; rotation 0, crop and scale kept. */
Transform fit_canvas(const preview_edit::Item &item, layout::Size canvas);

/** Bounds SCALE_OUTER of the canvas size at (0,0), centred inside; rotation 0, crop and scale kept. */
Transform fill_canvas(const preview_edit::Item &item, layout::Size canvas);

/** Scale 1, position 0, no bounds, no crop, rotation 0, alignment top-left. */
Transform reset_transform();

/** New position: the item's axis-aligned extent is centred on the canvas, the vertical position is kept. */
preview_edit::Vec2 center_horizontally(const preview_edit::Item &item, layout::Size canvas);

/** New position: the item's axis-aligned extent is centred on the canvas, the horizontal position is kept. */
preview_edit::Vec2 center_vertically(const preview_edit::Item &item, layout::Size canvas);

/** base if free, else "base 2", "base 3", ... (the OBS naming style). Comparison is exact. */
std::string unique_name(const std::string &base, const std::vector<std::string> &existing);

} // namespace smulti::item_actions
