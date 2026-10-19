// Pure item-action math for the vertical dock. No OBS calls.
#include "TestRunner.hpp"
#include "core/ItemActions.hpp"

#include <cmath>

using namespace smulti;
using item_actions::BoundsType;
using preview_edit::Item;

static bool near(float a, float b, float tol = 0.01f) { return std::fabs(a - b) <= tol; }

static Item webcam()
{
	Item it;
	it.id = 7;
	it.pos = {100, 200};
	it.rot = 30;
	it.scale = {0.5f, 0.5f};
	it.source = {1280, 720};
	it.crop = {10, 20, 30, 40};
	return it;
}

SMULTI_TEST(item_actions_fit_canvas)
{
	const auto t = item_actions::fit_canvas(webcam(), {1080, 1920});
	EXPECT_TRUE(t.bounds_type == BoundsType::ScaleInner);
	EXPECT_TRUE(near(t.bounds.x, 1080) && near(t.bounds.y, 1920));
	EXPECT_TRUE(near(t.pos.x, 0) && near(t.pos.y, 0));
	EXPECT_TRUE(t.bounds_alignment == 0); // centred inside the bounds
	EXPECT_TRUE(near(t.rot, 0));
	EXPECT_EQ(t.crop.left, 10); // crop is kept, the bounds fit the cropped source
	EXPECT_EQ(t.crop.bottom, 40);
	// zero canvas: no bounds, nothing to fit into
	EXPECT_TRUE(item_actions::fit_canvas(webcam(), {0, 1920}).bounds_type == BoundsType::None);
}

SMULTI_TEST(item_actions_fill_canvas)
{
	const auto t = item_actions::fill_canvas(webcam(), {1080, 1920});
	EXPECT_TRUE(t.bounds_type == BoundsType::ScaleOuter);
	EXPECT_TRUE(near(t.bounds.x, 1080) && near(t.bounds.y, 1920));
	EXPECT_TRUE(near(t.pos.x, 0) && near(t.pos.y, 0));
	EXPECT_TRUE(t.bounds_alignment == 0);
	EXPECT_TRUE(near(t.rot, 0));
	EXPECT_EQ(t.crop.top, 20);
	EXPECT_TRUE(item_actions::fill_canvas(webcam(), {1080, 0}).bounds_type == BoundsType::None);
}

SMULTI_TEST(item_actions_reset)
{
	const auto t = item_actions::reset_transform();
	EXPECT_TRUE(near(t.scale.x, 1) && near(t.scale.y, 1));
	EXPECT_TRUE(near(t.pos.x, 0) && near(t.pos.y, 0));
	EXPECT_TRUE(t.bounds_type == BoundsType::None);
	EXPECT_TRUE(near(t.rot, 0));
	EXPECT_TRUE(t.crop.left == 0 && t.crop.top == 0 && t.crop.right == 0 && t.crop.bottom == 0);
	EXPECT_TRUE(t.alignment == (preview_edit::AlignLeft | preview_edit::AlignTop));
}

SMULTI_TEST(item_actions_center_h)
{
	Item it;
	it.pos = {0, 300};
	it.source = {400, 200};
	it.scale = {1, 1}; // box x 0..400
	auto p = item_actions::center_horizontally(it, {1080, 1920});
	EXPECT_TRUE(near(p.x, 340)); // 340..740, centre 540
	EXPECT_TRUE(near(p.y, 300)); // vertical untouched

	// bounds item, centre alignment: the pos is the box centre, so it moves to 540
	Item b;
	b.pos = {100, 50};
	b.alignment = 0;
	b.has_bounds = true;
	b.bounds = {200, 100};
	b.source = {10, 10};
	p = item_actions::center_horizontally(b, {1080, 1920});
	EXPECT_TRUE(near(p.x, 540) && near(p.y, 50));
}

SMULTI_TEST(item_actions_center_v)
{
	Item it;
	it.pos = {123, 0};
	it.source = {400, 200};
	it.scale = {1, 1}; // box y 0..200
	const auto p = item_actions::center_vertically(it, {1080, 1920});
	EXPECT_TRUE(near(p.y, 860)); // 860..1060, centre 960
	EXPECT_TRUE(near(p.x, 123));

	// rotated 90 degrees about the top-left: the extent is used, not the unrotated box
	Item r = it;
	r.rot = 90;
	r.pos = {500, 0}; // covers x 300..500, y 0..400
	const auto q = item_actions::center_vertically(r, {1080, 1920});
	EXPECT_TRUE(near(q.y, 760)); // 760..1160, centre 960
}

SMULTI_TEST(item_actions_unique_name)
{
	EXPECT_EQ(item_actions::unique_name("Webcam", {}), std::string("Webcam"));
	EXPECT_EQ(item_actions::unique_name("Webcam", {"Webcam"}), std::string("Webcam 2"));
	EXPECT_EQ(item_actions::unique_name("Webcam", {"Webcam", "Webcam 2", "Webcam 4"}), std::string("Webcam 3"));
	EXPECT_EQ(item_actions::unique_name("Webcam", {"webcam"}), std::string("Webcam")); // exact comparison
}
