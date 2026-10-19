// Pure transform math for the vertical editor. No OBS calls.
#include "TestRunner.hpp"
#include "core/LayoutMath.hpp"

#include <cmath>

using smulti::layout::clamp_crop;
using smulti::layout::Crop;
using smulti::layout::fill;
using smulti::layout::fit;
using smulti::layout::ItemTransform;
using smulti::layout::Size;

static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

static bool no_crop(const Crop &c) { return c.left == 0 && c.top == 0 && c.right == 0 && c.bottom == 0; }

SMULTI_TEST(layout_fit_landscape_into_portrait)
{
	ItemTransform t = fit({1920, 1080}, {1080, 1920});
	EXPECT_TRUE(near(t.scale_x, 0.5625f, 1e-4f));
	EXPECT_TRUE(near(t.scale_y, 0.5625f, 1e-4f));
	EXPECT_TRUE(near(t.pos_x, 0.0f, 0.5f));
	EXPECT_TRUE(near(t.pos_y, 656.25f, 0.5f));
	EXPECT_TRUE(no_crop(t.crop));
}

SMULTI_TEST(layout_fill_landscape_into_portrait_center_crops)
{
	ItemTransform t = fill({1920, 1080}, {1080, 1920});
	EXPECT_TRUE(near(t.scale_x, 1.7778f, 1e-3f));
	EXPECT_TRUE(near(t.scale_y, 1.7778f, 1e-3f));
	EXPECT_TRUE(near(t.pos_x, 0.0f, 1e-4f));
	EXPECT_TRUE(near(t.pos_y, 0.0f, 1e-4f));
	EXPECT_TRUE(std::abs(t.crop.left - 656) <= 1);
	EXPECT_TRUE(std::abs(t.crop.right - 656) <= 1);
	EXPECT_EQ(t.crop.left, t.crop.right);
	EXPECT_EQ(t.crop.top, 0);
	EXPECT_EQ(t.crop.bottom, 0);
}

SMULTI_TEST(layout_fit_same_aspect_is_exact)
{
	ItemTransform t = fit({1080, 1920}, {1080, 1920});
	EXPECT_TRUE(near(t.scale_x, 1.0f, 1e-6f));
	EXPECT_TRUE(near(t.scale_y, 1.0f, 1e-6f));
	EXPECT_TRUE(near(t.pos_x, 0.0f, 1e-6f));
	EXPECT_TRUE(near(t.pos_y, 0.0f, 1e-6f));
	EXPECT_TRUE(no_crop(t.crop));
}

SMULTI_TEST(layout_clamp_crop_never_exceeds_source)
{
	Crop c = clamp_crop({1500, -5, 1500, 0}, {1920, 1080});
	EXPECT_TRUE(c.left >= 0 && c.right >= 0);
	EXPECT_TRUE(c.left + c.right <= 1919);
	EXPECT_EQ(c.top, 0);
	EXPECT_TRUE(c.bottom >= 0 && c.top + c.bottom <= 1079);
}

SMULTI_TEST(layout_zero_source_size_is_identity)
{
	ItemTransform a = fit({0, 0}, {1080, 1920});
	ItemTransform b = fill({0, 0}, {1080, 1920});
	for (const ItemTransform &t : {a, b}) {
		EXPECT_TRUE(near(t.scale_x, 1.0f, 1e-6f));
		EXPECT_TRUE(near(t.scale_y, 1.0f, 1e-6f));
		EXPECT_TRUE(near(t.pos_x, 0.0f, 1e-6f));
		EXPECT_TRUE(near(t.pos_y, 0.0f, 1e-6f));
		EXPECT_TRUE(no_crop(t.crop));
	}
}
