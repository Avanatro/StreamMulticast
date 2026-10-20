// Pure safe-zone table and snapping tests. No OBS calls, no Qt.
#include "TestRunner.hpp"
#include "core/SafeZone.hpp"

#include <cmath>
#include <vector>

namespace sz = smulti::safe_zone;
namespace pe = smulti::preview_edit;

static const smulti::layout::Size kCanvas{1080, 1920};

SMULTI_TEST(safe_zone_tiktok_1080x1920_matches_table)
{
	const sz::Zone z = sz::safe_zone_for(sz::Profile::TikTok, kCanvas);
	EXPECT_TRUE(z.active);
	EXPECT_EQ(z.unsafe_top.y, 0);
	EXPECT_EQ(z.unsafe_top.h, 240);
	EXPECT_EQ(z.unsafe_top.w, 1080);
	EXPECT_EQ(z.unsafe_bottom.y, 1920 - 660);
	EXPECT_EQ(z.unsafe_bottom.h, 660);
	EXPECT_EQ(z.unsafe_left.x, 0);
	EXPECT_EQ(z.unsafe_left.w, 120);
	EXPECT_EQ(z.unsafe_right.x, 1080 - 120);
	EXPECT_EQ(z.unsafe_right.w, 120);
	EXPECT_EQ(z.unsafe_left.y, 240);
	EXPECT_EQ(z.unsafe_left.h, 1920 - 240 - 660);
	EXPECT_TRUE(z.has_action_column);
	EXPECT_EQ(z.action_column.x, 900);
	EXPECT_EQ(z.action_column.w, 180);
	EXPECT_EQ(z.action_column.y, 840);
	EXPECT_EQ(z.action_column.h, 420);
	EXPECT_EQ(z.safe.x, 120);
	EXPECT_EQ(z.safe.y, 240);
	EXPECT_EQ(z.safe.w, 840);
	EXPECT_EQ(z.safe.h, 1020);

	const sz::Zone s = sz::safe_zone_for(sz::Profile::Shorts, kCanvas);
	EXPECT_TRUE(s.active);
	EXPECT_EQ(s.unsafe_top.h, 288);
	EXPECT_EQ(s.unsafe_bottom.h, 672);
	EXPECT_EQ(s.unsafe_left.w, 48);
	EXPECT_EQ(s.unsafe_right.w, 192);
	EXPECT_TRUE(!(s.has_action_column));
	EXPECT_EQ(s.safe.w, 840);
	EXPECT_EQ(s.safe.h, 960);
}

SMULTI_TEST(safe_zone_scales_with_canvas)
{
	/* Half size: every number halves. */
	const sz::Zone h = sz::safe_zone_for(sz::Profile::TikTok, {540, 960});
	EXPECT_EQ(h.unsafe_top.h, 120);
	EXPECT_EQ(h.unsafe_bottom.h, 330);
	EXPECT_EQ(h.unsafe_left.w, 60);
	EXPECT_EQ(h.action_column.x, 450);
	EXPECT_EQ(h.action_column.y, 420);
	EXPECT_EQ(h.safe.w, 420);
	EXPECT_EQ(h.safe.h, 510);
	/* Axes scale independently (non-9:16 canvas). */
	const sz::Zone w = sz::safe_zone_for(sz::Profile::TikTok, {2160, 1920});
	EXPECT_EQ(w.unsafe_left.w, 240);
	EXPECT_EQ(w.unsafe_top.h, 240);
}

SMULTI_TEST(safe_zone_off_is_empty)
{
	const sz::Zone z = sz::safe_zone_for(sz::Profile::Off, kCanvas);
	EXPECT_TRUE(!(z.active));
	EXPECT_TRUE(!(z.has_action_column));
	EXPECT_EQ(z.safe.w, 0);
	EXPECT_EQ(z.unsafe_top.h, 0);
	const sz::Zone e = sz::safe_zone_for(sz::Profile::TikTok, {0, 0});
	EXPECT_TRUE(!(e.active));
	EXPECT_TRUE(!(sz::safe_zone_snap_item(z).visible));
}

SMULTI_TEST(safe_zone_snap_item_matches_safe_rect)
{
	const sz::Zone z = sz::safe_zone_for(sz::Profile::TikTok, kCanvas);
	const pe::Item it = sz::safe_zone_snap_item(z);
	EXPECT_TRUE(it.visible);
	EXPECT_TRUE(it.pos.x == 120.0f && it.pos.y == 240.0f);
	EXPECT_EQ(it.source.w, 840u);
	EXPECT_EQ(it.source.h, 1020u);
	EXPECT_TRUE(it.scale.x == 1.0f && it.scale.y == 1.0f);
	EXPECT_TRUE(it.rot == 0.0f);
	EXPECT_TRUE(!(it.has_bounds));
	const auto c = pe::corners(it);
	EXPECT_TRUE(c[2].x == 960.0f && c[2].y == 1260.0f);
}

SMULTI_TEST(safe_zone_profile_string_roundtrip)
{
	for (sz::Profile p : {sz::Profile::Off, sz::Profile::TikTok, sz::Profile::Shorts})
		EXPECT_TRUE(sz::profile_from_string(sz::profile_to_string(p)) == p);
	EXPECT_EQ(std::string(sz::profile_to_string(sz::Profile::TikTok)), "tiktok");
	EXPECT_EQ(std::string(sz::profile_to_string(sz::Profile::Shorts)), "shorts");
	EXPECT_EQ(std::string(sz::profile_to_string(sz::Profile::Off)), "off");
	EXPECT_TRUE(sz::profile_from_string("") == sz::Profile::Off);
	EXPECT_TRUE(sz::profile_from_string("instagram") == sz::Profile::Off);
}

SMULTI_TEST(preview_edit_move_snaps_to_safe_zone_edge)
{
	const sz::Zone z = sz::safe_zone_for(sz::Profile::TikTok, kCanvas);
	std::vector<pe::Item> others{sz::safe_zone_snap_item(z)};
	pe::Item cam;
	cam.id = 1;
	cam.pos = {100, 500};
	cam.source = {200, 200};
	/* Drag so the right edge lands at 957 (safe right edge 960, threshold 12): x = 757 -> snaps to 760. */
	const pe::Vec2 p = pe::move_position(cam, {657, 0}, kCanvas, others, 12.0f, false);
	EXPECT_TRUE(std::fabs(p.x - 760.0f) < 0.01f);
	/* Left edge near the safe left edge: x 125 -> 120. */
	const pe::Vec2 q = pe::move_position(cam, {25, 0}, kCanvas, others, 12.0f, false);
	EXPECT_TRUE(std::fabs(q.x - 120.0f) < 0.01f);
	/* Ctrl disables snapping. */
	const pe::Vec2 r = pe::move_position(cam, {657, 0}, kCanvas, others, 12.0f, true);
	EXPECT_TRUE(std::fabs(r.x - 757.0f) < 0.01f);
	/* Without the safe zone item, no snap at 757. */
	const pe::Vec2 n = pe::move_position(cam, {657, 0}, kCanvas, {}, 12.0f, false);
	EXPECT_TRUE(std::fabs(n.x - 757.0f) < 0.01f);
}
