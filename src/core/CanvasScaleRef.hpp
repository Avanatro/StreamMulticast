/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <cstddef>
#include <cstdint>

struct obs_data;
typedef struct obs_data obs_data_t;

namespace smulti {

/**
 * Scene item transform helpers for canvases of different sizes. Pure: only obs_data and math, no libobs
 * scene calls.
 *
 * libobs (relative coordinates, the default) stores an item's position and bounds relative to the
 * height of the canvas of its scene, and its scale relative to `scale_ref`, the canvas size captured
 * when the item was created. The displayed scale is `scale_rel * canvas_height / scale_ref.y`, so a
 * `scale_ref` of {0,0} yields an infinite scale and the item is not drawn.
 */
struct Vec2d {
	double x = 0.0;
	double y = 0.0;
};

/** True when `ref` cannot be used as a divisor: either component zero, negative or not finite. */
bool scale_ref_needs_repair(Vec2d ref);

/**
 * Rewrites the `scale_ref` of every saved item in `scene_settings["items"]` that needs repair to
 * {canvas_w, canvas_h}. Items without a `scale_ref` (absolute coordinates) are left alone. Returns the
 * number of items rewritten; 0 when the canvas size is zero or nothing needs repair (idempotent).
 */
size_t repair_scene_scale_refs(obs_data_t *scene_settings, uint32_t canvas_w, uint32_t canvas_h);

/** The relative transform of one saved scene item, as libobs scene_save writes it. */
struct RelativeItemTransform {
	Vec2d pos_rel;
	Vec2d scale_rel{1.0, 1.0};
	Vec2d bounds_rel;
	Vec2d scale_ref;
	bool nested_scene = false; ///< the item shows a scene: libobs never rescales its scale by scale_ref
};

/** Absolute values for obs_sceneitem_set_info2 on an item in a canvas of the given size. */
struct AbsoluteItemTransform {
	Vec2d pos;
	Vec2d scale{1.0, 1.0};
	Vec2d bounds;
};

/**
 * Reads pos_rel / scale_rel / scale_ref (and bounds_rel when present) of one saved item. False when the
 * item was saved in absolute-coordinate mode (no pos_rel, scale_rel or scale_ref user value).
 */
bool read_relative_transform(obs_data_t *item_data, RelativeItemTransform &out);

/**
 * The absolute values that make obs_sceneitem_set_info2 reproduce `rel` on an item of a canvas of
 * canvas_w x canvas_h whose own scale_ref is that canvas size (an item just added with obs_scene_add):
 * the exact inverse of libobs pos_from_absolute / size_from_absolute / item_relative_scale. The scale
 * keeps the displayed size of the original (`scale_rel * canvas_h / scale_ref.y`); a scale_ref that
 * needs repair is treated as the target canvas size. A zero canvas yields the identity transform.
 */
AbsoluteItemTransform relative_to_absolute(const RelativeItemTransform &rel, uint32_t canvas_w, uint32_t canvas_h);

} // namespace smulti
