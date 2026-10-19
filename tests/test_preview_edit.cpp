// Pure click-and-drag editing math for the vertical preview. No OBS calls, no Qt.
#include "TestRunner.hpp"
#include "core/PreviewEdit.hpp"

#include <cmath>
#include <vector>

namespace pe = smulti::preview_edit;
using pe::Handle;
using pe::Item;
using pe::Vec2;

static bool near(float a, float b, float tol = 0.01f) { return std::fabs(a - b) <= tol; }
static bool near_v(Vec2 a, Vec2 b, float tol = 0.01f) { return near(a.x, b.x, tol) && near(a.y, b.y, tol); }

/* Source w x h, scale 1, top-left aligned, no bounds, no rotation. */
static Item box_item(int64_t id, float x, float y, uint32_t w, uint32_t h)
{
	Item it;
	it.id = id;
	it.pos = {x, y};
	it.source = {w, h};
	return it;
}

static const smulti::layout::Size kCanvas{1080, 1920};

SMULTI_TEST(preview_edit_map_roundtrip)
{
	/* 1080x1920 into 400x400: scale 400/1920, 225 px wide, centred at x 87.5 -> 88 (lround, like the render). */
	const pe::View v = pe::make_view(kCanvas, {400, 400});
	EXPECT_TRUE(near_v(pe::canvas_to_widget(v, {0, 0}), {88, 0}));
	EXPECT_TRUE(near_v(pe::canvas_to_widget(v, {1080, 1920}), {88 + 225, 400}));
	for (Vec2 p : {Vec2{0, 0}, Vec2{540, 960}, Vec2{1080, 1920}, Vec2{12.5f, 1777.25f}}) {
		const Vec2 back = pe::widget_to_canvas(v, pe::canvas_to_widget(v, p));
		EXPECT_TRUE(near_v(back, p, 0.01f));
	}
	/* 8 widget px are 8 / (400/1920) = 38.4 canvas px. */
	EXPECT_TRUE(near(pe::widget_to_canvas_length(v, 8.0f), 38.4f, 0.2f));

	/* The viewport is the one the render uses (shared LayoutMath). */
	const smulti::layout::Rect r = smulti::layout::letterbox(kCanvas, {400, 400});
	EXPECT_EQ(r.x, 88);
	EXPECT_EQ(r.y, 0);
	EXPECT_EQ(r.w, 225);
	EXPECT_EQ(r.h, 400);

	/* Degenerate widget: mapping stays finite. */
	const pe::View z = pe::make_view(kCanvas, {0, 0});
	const Vec2 zp = pe::widget_to_canvas(z, {10, 10});
	EXPECT_TRUE(std::isfinite(zp.x) && std::isfinite(zp.y));
}

SMULTI_TEST(preview_edit_hit_topmost_wins)
{
	/* libobs order: bottom first. */
	std::vector<Item> items{box_item(1, 0, 0, 500, 500), box_item(2, 200, 200, 500, 500)};
	EXPECT_EQ(pe::hit_item(items, {300, 300}), (int64_t)2); // overlap: top wins
	EXPECT_EQ(pe::hit_item(items, {50, 50}), (int64_t)1);   // only the bottom one
	EXPECT_EQ(pe::hit_item(items, {900, 900}), (int64_t)0); // nothing
}

SMULTI_TEST(preview_edit_hit_skips_locked_hidden)
{
	std::vector<Item> items{box_item(1, 0, 0, 500, 500), box_item(2, 0, 0, 500, 500)};
	items[1].locked = true;
	EXPECT_EQ(pe::hit_item(items, {100, 100}), (int64_t)1);
	items[1].locked = false;
	items[1].visible = false;
	EXPECT_EQ(pe::hit_item(items, {100, 100}), (int64_t)1);
	items[0].locked = true;
	EXPECT_EQ(pe::hit_item(items, {100, 100}), (int64_t)0);

	/* A zero-sized (audio-only) item is never hit. */
	std::vector<Item> audio{box_item(3, 0, 0, 0, 0)};
	EXPECT_EQ(pe::hit_item(audio, {0, 0}), (int64_t)0);
}

SMULTI_TEST(preview_edit_hit_rotated_box)
{
	/* 100x100 centred at (500,500), rotated 45 degrees: a diamond reaching 70.7 px along the axes. */
	Item it = box_item(7, 500, 500, 100, 100);
	it.alignment = 0; // centre
	it.rot = 45.0f;
	std::vector<Item> items{it};
	EXPECT_EQ(pe::hit_item(items, {560, 500}), (int64_t)7); // outside the unrotated box, inside the diamond
	EXPECT_EQ(pe::hit_item(items, {540, 540}), (int64_t)0); // inside the unrotated box, outside the diamond
	EXPECT_EQ(pe::hit_item(items, {500, 500}), (int64_t)7);

	/* Corners follow libobs: canvas = pos + R(rot) * (unit * size - alignment origin). */
	Item r90 = box_item(8, 500, 500, 200, 100);
	r90.rot = 90.0f;
	const auto c = pe::corners(r90);
	EXPECT_TRUE(near_v(c[0], {500, 500})); // top-left
	EXPECT_TRUE(near_v(c[1], {500, 700})); // top-right: +x of the item points down
	EXPECT_TRUE(near_v(c[2], {400, 700})); // bottom-right
	EXPECT_TRUE(near_v(c[3], {400, 500})); // bottom-left
}

SMULTI_TEST(preview_edit_handle_hit)
{
	const pe::View v = pe::make_view(kCanvas, {1080, 1920}); // 1:1
	const Item it = box_item(1, 100, 100, 200, 100);
	EXPECT_EQ(pe::hit_handle(it, v, {302, 99}), Handle::TopRight);
	EXPECT_EQ(pe::hit_handle(it, v, {200, 204}), Handle::Bottom);
	EXPECT_EQ(pe::hit_handle(it, v, {96, 150}), Handle::Left);
	EXPECT_EQ(pe::hit_handle(it, v, {104, 104}), Handle::TopLeft);
	EXPECT_EQ(pe::hit_handle(it, v, {200, 150}), Handle::None); // inside, not on a handle
	EXPECT_EQ(pe::hit_handle(it, v, {310, 100}), Handle::None); // 10 px away > default radius 6

	/* The radius is in widget px: at a 0.5 view scale a canvas distance of 10 is 5 widget px. */
	const pe::View half = pe::make_view(kCanvas, {540, 960});
	EXPECT_EQ(pe::hit_handle(it, half, pe::canvas_to_widget(half, {310, 100})), Handle::TopRight);
}

SMULTI_TEST(preview_edit_move_snaps_to_edge)
{
	const float threshold = 8.0f;
	const Item it = box_item(1, 100, 300, 100, 100);
	/* left edge would land at x 5: snaps to the canvas edge 0; y stays */
	Vec2 p = pe::move_position(it, {-95, 0}, kCanvas, {}, threshold, false);
	EXPECT_TRUE(near_v(p, {0, 300}));
	/* bottom edge would land at 1915: snaps to 1920 */
	p = pe::move_position(it, {-100, 1515}, kCanvas, {}, threshold, false);
	EXPECT_TRUE(near_v(p, {0, 1820}));

	/* right edge at 797 snaps to another visible item's left edge at 800 (centre 747 is far from 540) */
	std::vector<Item> others{box_item(2, 800, 1000, 100, 100)};
	p = pe::move_position(it, {597, 0}, kCanvas, others, threshold, false);
	EXPECT_TRUE(near_v(p, {700, 300}));
	/* a hidden item is no snap target */
	others[0].visible = false;
	p = pe::move_position(it, {597, 0}, kCanvas, others, threshold, false);
	EXPECT_TRUE(near_v(p, {697, 300}));
	/* farther than the threshold: no snap */
	p = pe::move_position(it, {-80, 0}, kCanvas, {}, threshold, false);
	EXPECT_TRUE(near_v(p, {20, 300}));
}

SMULTI_TEST(preview_edit_move_snaps_to_center)
{
	const Item it = box_item(1, 0, 0, 100, 100);
	/* item centre would be at x 537: snaps to the centre line 540 */
	Vec2 p = pe::move_position(it, {487, 0}, kCanvas, {}, 8.0f, false);
	EXPECT_TRUE(near_v(p, {490, 0}));
	/* vertical centre line 960 */
	p = pe::move_position(it, {200, 914}, kCanvas, {}, 8.0f, false);
	EXPECT_TRUE(near_v(p, {200, 910}));
}

SMULTI_TEST(preview_edit_move_ctrl_disables_snap)
{
	const Item it = box_item(1, 100, 300, 100, 100);
	Vec2 p = pe::move_position(it, {-95, 0}, kCanvas, {}, 8.0f, true);
	EXPECT_TRUE(near_v(p, {5, 300}));
	p = pe::move_position(box_item(1, 0, 0, 100, 100), {487, 0}, kCanvas, {}, 8.0f, true);
	EXPECT_TRUE(near_v(p, {487, 0}));
}

SMULTI_TEST(preview_edit_resize_corner_keeps_aspect)
{
	const Item it = box_item(1, 100, 100, 200, 100);
	/* bottom-right from (300,200) to (500,220): x grows 2x, y 1.2x -> both 2x, top-left fixed */
	Item r = pe::resize(it, Handle::BottomRight, {500, 220}, false);
	EXPECT_TRUE(near_v(r.scale, {2, 2}));
	EXPECT_TRUE(near_v(r.pos, {100, 100}));

	/* top-left corner: the bottom-right corner (300,200) stays */
	r = pe::resize(it, Handle::TopLeft, {0, 150}, false);
	EXPECT_TRUE(near_v(r.scale, {1.5f, 1.5f}));
	EXPECT_TRUE(near_v(pe::corners(r)[2], {300, 200}));

	/* centre-aligned item: the opposite corner still stays put */
	Item c = it;
	c.alignment = 0;
	const Vec2 tl_before = pe::corners(c)[0];
	r = pe::resize(c, Handle::BottomRight, pe::corners(c)[2] + Vec2{200, 0}, false);
	EXPECT_TRUE(near_v(r.scale, {2, 2}));
	EXPECT_TRUE(near_v(pe::corners(r)[0], tl_before));
}

SMULTI_TEST(preview_edit_resize_shift_free)
{
	const Item it = box_item(1, 100, 100, 200, 100);
	Item r = pe::resize(it, Handle::BottomRight, {500, 220}, true);
	EXPECT_TRUE(near_v(r.scale, {2, 1.2f}));
	EXPECT_TRUE(near_v(r.pos, {100, 100}));
}

SMULTI_TEST(preview_edit_resize_edge_one_axis)
{
	const Item it = box_item(1, 100, 100, 200, 100);
	/* right edge to x 400 (y ignored) */
	Item r = pe::resize(it, Handle::Right, {400, 999}, false);
	EXPECT_TRUE(near_v(r.scale, {1.5f, 1}));
	EXPECT_TRUE(near_v(r.pos, {100, 100}));
	/* left edge to x 50: the right edge (300) stays */
	r = pe::resize(it, Handle::Left, {50, 150}, false);
	EXPECT_TRUE(near_v(r.scale, {1.25f, 1}));
	EXPECT_TRUE(near_v(r.pos, {50, 100}));
	/* bottom edge */
	r = pe::resize(it, Handle::Bottom, {0, 300}, true);
	EXPECT_TRUE(near_v(r.scale, {1, 2}));

	/* rotated 90 degrees: the item's +x axis points down, so dragging its right edge down widens it */
	Item rot = box_item(2, 500, 500, 200, 100);
	rot.rot = 90.0f;
	r = pe::resize(rot, Handle::Right, {450, 800}, false);
	EXPECT_TRUE(near_v(r.scale, {1.5f, 1}));
	EXPECT_TRUE(near_v(r.pos, {500, 500}));
	/* 180 and 270 keep the opposite edge too */
	for (float deg : {180.0f, 270.0f}) {
		Item q = rot;
		q.rot = deg;
		const auto before = pe::corners(q);
		const Vec2 left_mid{(before[0].x + before[3].x) / 2, (before[0].y + before[3].y) / 2};
		const Vec2 right_mid{(before[1].x + before[2].x) / 2, (before[1].y + before[2].y) / 2};
		const Vec2 dir{(right_mid.x - left_mid.x) / 200.0f, (right_mid.y - left_mid.y) / 200.0f};
		r = pe::resize(q, Handle::Right, right_mid + Vec2{dir.x * 100, dir.y * 100}, false);
		EXPECT_TRUE(near_v(r.scale, {1.5f, 1}));
		const auto after = pe::corners(r);
		EXPECT_TRUE(near_v({(after[0].x + after[3].x) / 2, (after[0].y + after[3].y) / 2}, left_mid));
	}
}

SMULTI_TEST(preview_edit_resize_bounds_item)
{
	Item it = box_item(1, 100, 100, 1920, 1080);
	it.has_bounds = true;
	it.bounds = {400, 300};
	/* corner keeps the bounds' aspect: 500/400 = 1.25 wins over 300/300 */
	Item r = pe::resize(it, Handle::BottomRight, {600, 400}, false);
	EXPECT_TRUE(near_v(r.bounds, {500, 375}));
	EXPECT_TRUE(near_v(r.scale, {1, 1})); // scale untouched: libobs fits the source into the bounds
	EXPECT_TRUE(near_v(r.pos, {100, 100}));
	r = pe::resize(it, Handle::BottomRight, {600, 400}, true);
	EXPECT_TRUE(near_v(r.bounds, {500, 300}));
	r = pe::resize(it, Handle::Top, {0, 0}, false);
	EXPECT_TRUE(near_v(r.bounds, {400, 400}));
	EXPECT_TRUE(near_v(r.pos, {100, 0}));
}

SMULTI_TEST(preview_edit_resize_min_size)
{
	const Item it = box_item(1, 100, 100, 200, 100);
	/* free: each axis clamps to 8 canvas px */
	Item r = pe::resize(it, Handle::BottomRight, {101, 101}, true);
	EXPECT_TRUE(near(pe::box_size(r).x, 8));
	EXPECT_TRUE(near(pe::box_size(r).y, 8));
	EXPECT_TRUE(near_v(r.pos, {100, 100}));
	/* aspect: the smaller side reaches 8, the other keeps the ratio */
	r = pe::resize(it, Handle::BottomRight, {101, 101}, false);
	EXPECT_TRUE(near(pe::box_size(r).x, 16));
	EXPECT_TRUE(near(pe::box_size(r).y, 8));
	/* dragging past the opposite edge does not flip, it clamps */
	r = pe::resize(it, Handle::Right, {0, 150}, false);
	EXPECT_TRUE(near(pe::box_size(r).x, 8));
	EXPECT_TRUE(near_v(r.pos, {100, 100}));
	/* zero-sized source cannot be resized */
	const Item none = box_item(2, 0, 0, 0, 0);
	r = pe::resize(none, Handle::BottomRight, {50, 50}, false);
	EXPECT_TRUE(near_v(r.scale, {1, 1}));
}

SMULTI_TEST(preview_edit_nudge)
{
	EXPECT_TRUE(near_v(pe::nudge({10, 10}, 1, 0, false), {11, 10}));
	EXPECT_TRUE(near_v(pe::nudge({10, 10}, 0, -1, true), {10, 0}));
	EXPECT_TRUE(near_v(pe::nudge({10, 10}, -1, 1, true), {0, 20}));
	EXPECT_TRUE(near_v(pe::nudge({10, 10}, 0, 0, false), {10, 10}));
}

SMULTI_TEST(preview_edit_nudge_burst_coalesces_undo)
{
	pe::NudgeBurst b;
	/* first nudge of a burst captures the undo step, the next ones within 1 s do not */
	EXPECT_TRUE(pe::nudge_begins_step(b, "scene-a", 5, 1000));
	EXPECT_TRUE(!pe::nudge_begins_step(b, "scene-a", 5, 1200));
	EXPECT_TRUE(!pe::nudge_begins_step(b, "scene-a", 5, 2100)); // 900 ms after the previous one
	/* idle for 1 s or more: new step */
	EXPECT_TRUE(pe::nudge_begins_step(b, "scene-a", 5, 3100));
	/* another item or another scene: new step */
	EXPECT_TRUE(pe::nudge_begins_step(b, "scene-a", 6, 3200));
	EXPECT_TRUE(pe::nudge_begins_step(b, "scene-b", 6, 3300));
	/* a drag (or undo) ends the burst */
	pe::end_nudge_burst(b);
	EXPECT_TRUE(pe::nudge_begins_step(b, "scene-b", 6, 3400));
	/* clock going backwards never merges */
	EXPECT_TRUE(pe::nudge_begins_step(b, "scene-b", 6, 100));
}

SMULTI_TEST(preview_edit_drag_needs_left_button)
{
	/* a move event without the left button (release lost to Alt+Tab or a popup) ends the drag */
	EXPECT_TRUE(pe::drag_continues(true));
	EXPECT_TRUE(!pe::drag_continues(false));
}
