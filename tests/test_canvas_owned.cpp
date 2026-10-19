// Pure decisions for the plugin-owned vertical canvas. No OBS calls.
#include "TestRunner.hpp"
#include "core/OwnedCanvasPolicy.hpp"

#include <string>
#include <utility>
#include <vector>

using smulti::CanvasInfo;
using smulti::decide_owned_canvas;
using smulti::derive_vertical_geometry;
using smulti::endpoints_to_stop_on_canvas_removed;
using smulti::OwnedCanvasAction;
using smulti::VideoGeometry;

SMULTI_TEST(canvas_owned_creates_when_absent)
{
	std::vector<CanvasInfo> canvases = {{"main-uuid", "Main", true, true}};
	EXPECT_TRUE(decide_owned_canvas(canvases).action == OwnedCanvasAction::Create);
	EXPECT_TRUE(decide_owned_canvas({}).action == OwnedCanvasAction::Create);
}

SMULTI_TEST(canvas_owned_reuses_existing_by_name)
{
	std::vector<CanvasInfo> canvases = {
		{"main-uuid", "Main", true, true},
		{"vert-uuid", "StreamMulticast Vertical", true, false},
	};
	auto d = decide_owned_canvas(canvases);
	EXPECT_TRUE(d.action == OwnedCanvasAction::Reuse);
	EXPECT_TRUE(d.uuid == "vert-uuid");
}

SMULTI_TEST(canvas_owned_reuses_existing_by_name_and_resets_video)
{
	std::vector<CanvasInfo> canvases = {
		{"main-uuid", "Main", true, true},
		{"vert-uuid", "StreamMulticast Vertical", false, false},
	};
	auto d = decide_owned_canvas(canvases);
	EXPECT_TRUE(d.action == OwnedCanvasAction::ReuseAndResetVideo);
	EXPECT_TRUE(d.uuid == "vert-uuid");
}

SMULTI_TEST(canvas_owned_ignores_similar_names)
{
	std::vector<CanvasInfo> canvases = {
		{"main-uuid", "Main", true, true},
		{"a", "StreamMulticast Vertical 2", true, false},
		{"b", "Aitum Vertical", true, false},
	};
	EXPECT_TRUE(decide_owned_canvas(canvases).action == OwnedCanvasAction::Create);
}

SMULTI_TEST(canvas_owned_geometry_from_main_1080p60)
{
	VideoGeometry main{1920, 1080, 1920, 1080, 60, 1};
	VideoGeometry v = derive_vertical_geometry(main);
	EXPECT_TRUE(v.base_width == 1080 && v.base_height == 1920);
	EXPECT_TRUE(v.output_width == 1080 && v.output_height == 1920);
	EXPECT_TRUE(v.fps_num == 60 && v.fps_den == 1);
}

SMULTI_TEST(canvas_owned_removal_stops_bound_endpoints)
{
	std::vector<std::pair<std::string, std::string>> eps = {
		{"ep1", "vert-uuid"},
		{"ep2", "other-uuid"},
		{"ep3", "vert-uuid"},
	};
	auto ids = endpoints_to_stop_on_canvas_removed(eps, "vert-uuid");
	EXPECT_TRUE(ids.size() == 2);
	EXPECT_TRUE(ids[0] == "ep1" && ids[1] == "ep3");
	EXPECT_TRUE(endpoints_to_stop_on_canvas_removed(eps, "").empty());
}
