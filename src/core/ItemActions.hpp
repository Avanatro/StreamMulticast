/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "PreviewEdit.hpp"

#include <cstdint>
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

/** One item of a candidate scene (adapter output): which scene, which item, which source, locked or not. */
struct SceneItemRef {
	std::string scene_uuid;
	std::string scene_name;
	int64_t item_id = 0;
	std::string source_uuid;
	bool locked = false;
};

struct CopyTarget {
	std::string scene_uuid;
	int64_t item_id = 0;
};

struct CopySkip {
	std::string scene_name;
	std::string reason;
	bool locked = false; ///< skipped because the item is locked (counted by skipped_locked_count)
};

struct CopyPlan {
	std::vector<CopyTarget> targets; ///< ordered by scene name
	std::vector<CopySkip> skipped;   ///< ordered by scene name
};

/**
 * Scenes that get the origin item's transform: every scene other than the origin that holds source_uuid
 * exactly once and unlocked. A scene with the source 2+ times is skipped ("source appears N times", never
 * guessed), a locked one is skipped ("locked"). Scenes without the source are neither. Sources match by
 * uuid only; an empty source_uuid matches nothing.
 */
CopyPlan plan_copy_transform(const std::string &source_uuid, const std::string &origin_scene_uuid,
			     const std::vector<SceneItemRef> &candidates);

/** How many scenes plan_copy_transform skipped because the item is locked. */
size_t skipped_locked_count(const CopyPlan &plan);

} // namespace smulti::item_actions
