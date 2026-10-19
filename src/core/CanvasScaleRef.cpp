/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "CanvasScaleRef.hpp"

#include <obs-data.h>

#include <cmath>

namespace smulti {

namespace {

/* libobs saves a vec2 as an object {x, y} (obs_data_set_vec2). */
Vec2d get_vec(obs_data_t *data, const char *name)
{
	Vec2d v;
	obs_data_t *obj = obs_data_get_obj(data, name);
	if (obj) {
		v.x = obs_data_get_double(obj, "x");
		v.y = obs_data_get_double(obj, "y");
		obs_data_release(obj);
	}
	return v;
}

void set_vec(obs_data_t *data, const char *name, Vec2d v)
{
	obs_data_t *obj = obs_data_create();
	obs_data_set_double(obj, "x", v.x);
	obs_data_set_double(obj, "y", v.y);
	obs_data_set_obj(data, name, obj);
	obs_data_release(obj);
}

bool usable(double v)
{
	return std::isfinite(v) && v > 0.0;
}

/* Same condition as libobs scene_load_item for the relative path (else it loads the absolute values). */
bool is_relative_item(obs_data_t *item)
{
	return obs_data_has_user_value(item, "pos_rel") && obs_data_has_user_value(item, "scale_rel") &&
	       obs_data_has_user_value(item, "scale_ref");
}

} // namespace

bool scale_ref_needs_repair(Vec2d ref)
{
	return !(usable(ref.x) && usable(ref.y));
}

size_t repair_scene_scale_refs(obs_data_t *scene_settings, uint32_t canvas_w, uint32_t canvas_h)
{
	if (!scene_settings || canvas_w == 0 || canvas_h == 0)
		return 0;
	obs_data_array_t *items = obs_data_get_array(scene_settings, "items");
	if (!items)
		return 0;

	const Vec2d target{static_cast<double>(canvas_w), static_cast<double>(canvas_h)};
	size_t repaired = 0;
	const size_t count = obs_data_array_count(items);
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *item = obs_data_array_item(items, i);
		if (item && obs_data_has_user_value(item, "scale_ref") &&
		    scale_ref_needs_repair(get_vec(item, "scale_ref"))) {
			set_vec(item, "scale_ref", target);
			++repaired;
		}
		obs_data_release(item);
	}
	obs_data_array_release(items);
	return repaired;
}

bool item_scale_needs_repair(Vec2d scale)
{
	return !(std::isfinite(scale.x) && std::isfinite(scale.y) && scale.x != 0.0 && scale.y != 0.0);
}

std::optional<Vec2d> repaired_item_scale(obs_data_t *item_data, uint32_t canvas_w, uint32_t canvas_h,
					 bool nested_scene)
{
	if (!item_data || canvas_w == 0 || canvas_h == 0)
		return std::nullopt;

	const bool relative = is_relative_item(item_data); // decides which key libobs loads the scale from
	const char *loaded_key = relative ? "scale_rel" : "scale";
	const char *other_key = relative ? "scale" : "scale_rel";
	if (!item_scale_needs_repair(get_vec(item_data, loaded_key)))
		return std::nullopt;

	const Vec2d unit{1.0, 1.0};
	if (obs_data_get_int(item_data, "bounds_type") != 0) // OBS_BOUNDS_NONE == 0
		return unit;
	if (!obs_data_has_user_value(item_data, other_key))
		return unit;
	const Vec2d other = get_vec(item_data, other_key);
	if (item_scale_needs_repair(other))
		return unit;
	if (nested_scene)
		return other;

	const double h = static_cast<double>(canvas_h);
	Vec2d ref = get_vec(item_data, "scale_ref");
	if (scale_ref_needs_repair(ref))
		ref = Vec2d{static_cast<double>(canvas_w), h};
	/* displayed = scale_rel * h / ref.y (libobs item_canvas_scale) */
	const double factor = relative ? ref.y / h : h / ref.y;
	return Vec2d{other.x * factor, other.y * factor};
}

size_t repair_scene_item_scales(obs_data_t *scene_settings, uint32_t canvas_w, uint32_t canvas_h)
{
	if (!scene_settings || canvas_w == 0 || canvas_h == 0)
		return 0;
	obs_data_array_t *items = obs_data_get_array(scene_settings, "items");
	if (!items)
		return 0;

	size_t repaired = 0;
	const size_t count = obs_data_array_count(items);
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *item = obs_data_array_item(items, i);
		const std::optional<Vec2d> fixed = repaired_item_scale(item, canvas_w, canvas_h, false);
		if (fixed) {
			set_vec(item, is_relative_item(item) ? "scale_rel" : "scale", *fixed);
			++repaired;
		}
		obs_data_release(item);
	}
	obs_data_array_release(items);
	return repaired;
}

bool read_relative_transform(obs_data_t *item_data, RelativeItemTransform &out)
{
	if (!item_data || !is_relative_item(item_data))
		return false;
	out.pos_rel = get_vec(item_data, "pos_rel");
	out.scale_rel = get_vec(item_data, "scale_rel");
	out.scale_ref = get_vec(item_data, "scale_ref");
	out.bounds_rel = obs_data_has_user_value(item_data, "bounds_rel") ? get_vec(item_data, "bounds_rel") : Vec2d{};
	return true;
}

AbsoluteItemTransform relative_to_absolute(const RelativeItemTransform &rel, uint32_t canvas_w, uint32_t canvas_h)
{
	AbsoluteItemTransform abs;
	if (canvas_w == 0 || canvas_h == 0)
		return abs;
	const double w = static_cast<double>(canvas_w);
	const double h = static_cast<double>(canvas_h);

	/* Inverse of pos_from_absolute: rel.x = (2X - w) / h, rel.y = 2Y / h - 1 (no .5 nudging, so the
	 * round trip through obs_sceneitem_set_info2 is exact up to float precision). */
	abs.pos.x = (rel.pos_rel.x * h + w) / 2.0;
	abs.pos.y = (rel.pos_rel.y * h + h) / 2.0;
	/* Inverse of size_from_absolute: rel = 2 * size / h. */
	abs.bounds.x = rel.bounds_rel.x * h / 2.0;
	abs.bounds.y = rel.bounds_rel.y * h / 2.0;

	/* libobs item_canvas_scale: displayed scale = scale_rel * h / scale_ref.y; nested scenes are never
	 * rescaled. The new item's scale_ref is the target canvas, so set_info2 stores this value unchanged. */
	if (rel.nested_scene || scale_ref_needs_repair(rel.scale_ref)) {
		abs.scale = rel.scale_rel;
	} else {
		const double factor = h / rel.scale_ref.y;
		abs.scale.x = rel.scale_rel.x * factor;
		abs.scale.y = rel.scale_rel.y * factor;
	}
	return abs;
}

} // namespace smulti
