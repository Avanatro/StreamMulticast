// scale_ref repair and relative -> absolute item transform for imported scenes. obs_data only.
#include "TestRunner.hpp"
#include "core/CanvasScaleRef.hpp"

#include <obs-data.h>

#include <cmath>
#include <limits>

using smulti::AbsoluteItemTransform;
using smulti::read_relative_transform;
using smulti::relative_to_absolute;
using smulti::RelativeItemTransform;
using smulti::repair_scene_scale_refs;
using smulti::scale_ref_needs_repair;
using smulti::Vec2d;

namespace {

void set_vec(obs_data_t *d, const char *name, double x, double y)
{
	obs_data_t *obj = obs_data_create();
	obs_data_set_double(obj, "x", x);
	obs_data_set_double(obj, "y", y);
	obs_data_set_obj(d, name, obj);
	obs_data_release(obj);
}

Vec2d get_vec(obs_data_t *d, const char *name)
{
	Vec2d v;
	obs_data_t *obj = obs_data_get_obj(d, name);
	if (obj) {
		v.x = obs_data_get_double(obj, "x");
		v.y = obs_data_get_double(obj, "y");
		obs_data_release(obj);
	}
	return v;
}

/** Scene settings with one saved item per given scale_ref; ref_y < -1 means "no scale_ref key". */
obs_data_t *scene_with_refs(const std::vector<Vec2d> &refs)
{
	obs_data_t *scene = obs_data_create();
	obs_data_array_t *items = obs_data_array_create();
	for (const Vec2d &r : refs) {
		obs_data_t *it = obs_data_create();
		obs_data_set_string(it, "name", "Webcam");
		if (r.y >= -1.0)
			set_vec(it, "scale_ref", r.x, r.y);
		set_vec(it, "pos_rel", 0.1, -0.2);
		obs_data_array_push_back(items, it);
		obs_data_release(it);
	}
	obs_data_set_array(scene, "items", items);
	obs_data_array_release(items);
	return scene;
}

Vec2d item_ref(obs_data_t *scene, size_t i)
{
	obs_data_array_t *items = obs_data_get_array(scene, "items");
	obs_data_t *it = obs_data_array_item(items, i);
	Vec2d v = get_vec(it, "scale_ref");
	obs_data_release(it);
	obs_data_array_release(items);
	return v;
}

bool near(double a, double b)
{
	return std::fabs(a - b) < 1e-6;
}

/* libobs pos_from_absolute / size_from_absolute / item_relative_scale (obs-scene.c 31.1.1), used to
 * check that relative_to_absolute is their exact inverse. */
Vec2d lib_pos_from_abs(Vec2d v, double w, double h)
{
	return {(2 * v.x - w) / h, 2 * v.y / h - 1.0};
}
Vec2d lib_size_from_abs(Vec2d v, double h)
{
	return {2 * v.x / h, 2 * v.y / h};
}

} // namespace

SMULTI_TEST(canvas_scale_ref_repair_needs_repair_cases)
{
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double inf = std::numeric_limits<double>::infinity();
	EXPECT_TRUE(scale_ref_needs_repair({0.0, 0.0}));
	EXPECT_TRUE(scale_ref_needs_repair({1080.0, 0.0}));
	EXPECT_TRUE(scale_ref_needs_repair({0.0, 1920.0}));
	EXPECT_TRUE(scale_ref_needs_repair({-1080.0, 1920.0}));
	EXPECT_TRUE(scale_ref_needs_repair({1080.0, nan}));
	EXPECT_TRUE(scale_ref_needs_repair({inf, 1920.0}));
	EXPECT_TRUE(!scale_ref_needs_repair({1080.0, 1920.0}));
	EXPECT_TRUE(!scale_ref_needs_repair({1920.0, 1080.0}));
}

SMULTI_TEST(canvas_scale_ref_repair_rewrites_zero_refs_to_canvas_size)
{
	obs_data_t *scene = scene_with_refs({{0, 0}, {1080, 1920}, {0, 0}});
	EXPECT_EQ(repair_scene_scale_refs(scene, 1080, 1920), static_cast<size_t>(2));
	EXPECT_TRUE(near(item_ref(scene, 0).x, 1080) && near(item_ref(scene, 0).y, 1920));
	EXPECT_TRUE(near(item_ref(scene, 1).x, 1080) && near(item_ref(scene, 1).y, 1920));
	EXPECT_TRUE(near(item_ref(scene, 2).x, 1080) && near(item_ref(scene, 2).y, 1920));
	obs_data_release(scene);
}

SMULTI_TEST(canvas_scale_ref_repair_is_idempotent)
{
	obs_data_t *scene = scene_with_refs({{0, 0}, {0, 0}});
	EXPECT_EQ(repair_scene_scale_refs(scene, 1080, 1920), static_cast<size_t>(2));
	EXPECT_EQ(repair_scene_scale_refs(scene, 1080, 1920), static_cast<size_t>(0));
	obs_data_release(scene);
}

SMULTI_TEST(canvas_scale_ref_repair_keeps_valid_refs)
{
	/* A valid ref of another size is the original's reference, not a defect. */
	obs_data_t *scene = scene_with_refs({{1920, 1080}, {1080, 1920}});
	EXPECT_EQ(repair_scene_scale_refs(scene, 1080, 1920), static_cast<size_t>(0));
	EXPECT_TRUE(near(item_ref(scene, 0).x, 1920) && near(item_ref(scene, 0).y, 1080));
	obs_data_release(scene);
}

SMULTI_TEST(canvas_scale_ref_repair_ignores_items_without_ref_and_zero_canvas)
{
	obs_data_t *scene = scene_with_refs({{0, -5}}); // absolute-mode item: no scale_ref key
	EXPECT_EQ(repair_scene_scale_refs(scene, 1080, 1920), static_cast<size_t>(0));
	obs_data_array_t *items = obs_data_get_array(scene, "items");
	obs_data_t *it = obs_data_array_item(items, 0);
	EXPECT_TRUE(!obs_data_has_user_value(it, "scale_ref"));
	obs_data_release(it);
	obs_data_array_release(items);
	obs_data_release(scene);

	obs_data_t *broken = scene_with_refs({{0, 0}});
	EXPECT_EQ(repair_scene_scale_refs(broken, 0, 0), static_cast<size_t>(0));
	EXPECT_EQ(repair_scene_scale_refs(broken, 1080, 0), static_cast<size_t>(0));
	EXPECT_EQ(repair_scene_scale_refs(nullptr, 1080, 1920), static_cast<size_t>(0));
	obs_data_t *empty = obs_data_create();
	EXPECT_EQ(repair_scene_scale_refs(empty, 1080, 1920), static_cast<size_t>(0));
	obs_data_release(empty);
	obs_data_release(broken);
}

SMULTI_TEST(layout_import_rel_to_abs_inverts_libobs_conversion)
{
	RelativeItemTransform rel;
	rel.pos_rel = {-0.4375, 0.1458};
	rel.bounds_rel = {0.45, 0.8};
	rel.scale_rel = {0.5, 0.5};
	rel.scale_ref = {1080, 1920};
	const AbsoluteItemTransform abs = relative_to_absolute(rel, 1080, 1920);

	const Vec2d pos_back = lib_pos_from_abs(abs.pos, 1080, 1920);
	const Vec2d bounds_back = lib_size_from_abs(abs.bounds, 1920);
	EXPECT_TRUE(near(pos_back.x, rel.pos_rel.x) && near(pos_back.y, rel.pos_rel.y));
	EXPECT_TRUE(near(bounds_back.x, rel.bounds_rel.x) && near(bounds_back.y, rel.bounds_rel.y));
	/* Same reference as the target canvas: the scale is unchanged. */
	EXPECT_TRUE(near(abs.scale.x, 0.5) && near(abs.scale.y, 0.5));
	/* Spot values: pos_rel (0,0) is the canvas centre. */
	RelativeItemTransform centre;
	centre.scale_ref = {1080, 1920};
	const AbsoluteItemTransform c = relative_to_absolute(centre, 1080, 1920);
	EXPECT_TRUE(near(c.pos.x, 540) && near(c.pos.y, 960));
}

SMULTI_TEST(layout_import_scale_keeps_displayed_size_across_refs)
{
	/* Item made on a 1920x1080 canvas, imported into 1080x1920: displayed scale is rel * 1920 / 1080. */
	RelativeItemTransform rel;
	rel.scale_rel = {0.5, 0.25};
	rel.scale_ref = {1920, 1080};
	const AbsoluteItemTransform abs = relative_to_absolute(rel, 1080, 1920);
	EXPECT_TRUE(near(abs.scale.x, 0.5 * 1920.0 / 1080.0) && near(abs.scale.y, 0.25 * 1920.0 / 1080.0));
}

SMULTI_TEST(layout_import_nested_scene_and_broken_ref_scale_unscaled)
{
	RelativeItemTransform nested;
	nested.scale_rel = {0.7, 0.7};
	nested.scale_ref = {1920, 1080};
	nested.nested_scene = true;
	const AbsoluteItemTransform a = relative_to_absolute(nested, 1080, 1920);
	EXPECT_TRUE(near(a.scale.x, 0.7) && near(a.scale.y, 0.7));

	RelativeItemTransform broken;
	broken.scale_rel = {0.3, 0.3};
	broken.scale_ref = {0, 0}; // a copy of an already broken scene
	const AbsoluteItemTransform b = relative_to_absolute(broken, 1080, 1920);
	EXPECT_TRUE(std::isfinite(b.scale.x) && std::isfinite(b.scale.y));
	EXPECT_TRUE(near(b.scale.x, 0.3) && near(b.scale.y, 0.3));

	const AbsoluteItemTransform z = relative_to_absolute(broken, 0, 0);
	EXPECT_TRUE(near(z.scale.x, 1.0) && near(z.pos.x, 0.0) && near(z.bounds.y, 0.0));
}

SMULTI_TEST(layout_import_reads_saved_relative_item)
{
	obs_data_t *it = obs_data_create();
	set_vec(it, "pos_rel", -0.25, 0.5);
	set_vec(it, "scale_rel", 0.4, 0.6);
	set_vec(it, "scale_ref", 1080, 1920);
	set_vec(it, "bounds_rel", 0.9, 1.1);
	RelativeItemTransform rel;
	EXPECT_TRUE(read_relative_transform(it, rel));
	EXPECT_TRUE(near(rel.pos_rel.x, -0.25) && near(rel.pos_rel.y, 0.5));
	EXPECT_TRUE(near(rel.scale_rel.x, 0.4) && near(rel.scale_rel.y, 0.6));
	EXPECT_TRUE(near(rel.scale_ref.x, 1080) && near(rel.scale_ref.y, 1920));
	EXPECT_TRUE(near(rel.bounds_rel.x, 0.9) && near(rel.bounds_rel.y, 1.1));
	obs_data_release(it);

	/* Absolute-coordinate save: only pos/scale/bounds, no relative keys. */
	obs_data_t *abs_item = obs_data_create();
	set_vec(abs_item, "pos", 10, 20);
	set_vec(abs_item, "scale", 1, 1);
	RelativeItemTransform unused;
	EXPECT_TRUE(!read_relative_transform(abs_item, unused));
	EXPECT_TRUE(!read_relative_transform(nullptr, unused));
	obs_data_release(abs_item);
}
