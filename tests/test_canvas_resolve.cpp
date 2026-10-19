// Pure canvas reference resolution against a snapshot list. No OBS calls.
#include "TestRunner.hpp"
#include "core/CanvasResolve.hpp"

#include <string>
#include <vector>

using smulti::CanvasInfo;
using smulti::CanvasResolution;
using smulti::resolve_canvas;

namespace {
std::vector<CanvasInfo> sample_canvases()
{
	return {
		{"main-uuid", "Main", true, true},
		{"vert-uuid", "StreamMulticast Vertical", true, false},
		{"other-uuid", "Other", true, false},
	};
}
} // namespace

SMULTI_TEST(canvas_resolve_empty_ref_is_main)
{
	CanvasResolution r = resolve_canvas("", "", sample_canvases());
	EXPECT_TRUE(r.kind == CanvasResolution::Kind::Main);
}

SMULTI_TEST(canvas_resolve_uuid_hit)
{
	// The name in the ref differs from the listed name: the uuid wins.
	CanvasResolution r = resolve_canvas("vert-uuid", "Renamed Elsewhere", sample_canvases());
	EXPECT_TRUE(r.kind == CanvasResolution::Kind::Canvas);
	EXPECT_EQ(r.uuid, std::string("vert-uuid"));
}

SMULTI_TEST(canvas_resolve_uuid_miss_name_hit)
{
	CanvasResolution r = resolve_canvas("stale-uuid", "StreamMulticast Vertical", sample_canvases());
	EXPECT_TRUE(r.kind == CanvasResolution::Kind::Canvas);
	EXPECT_EQ(r.uuid, std::string("vert-uuid"));
	EXPECT_EQ(r.name, std::string("StreamMulticast Vertical"));
}

SMULTI_TEST(canvas_resolve_both_miss_refused)
{
	CanvasResolution r = resolve_canvas("stale-uuid", "Ghost Canvas", sample_canvases());
	EXPECT_TRUE(r.kind == CanvasResolution::Kind::Refused);
	EXPECT_TRUE(r.reason.find("not found") != std::string::npos);
	EXPECT_TRUE(r.reason.find("Ghost Canvas") != std::string::npos);
}

SMULTI_TEST(canvas_resolve_without_video_refused)
{
	std::vector<CanvasInfo> canvases = sample_canvases();
	canvases[1].has_video = false;
	CanvasResolution r = resolve_canvas("vert-uuid", "", canvases);
	EXPECT_TRUE(r.kind == CanvasResolution::Kind::Refused);
	EXPECT_TRUE(r.reason.find("no video mix") != std::string::npos);
}

SMULTI_TEST(canvas_resolve_ref_to_main_canvas_is_main)
{
	CanvasResolution r = resolve_canvas("main-uuid", "", sample_canvases());
	EXPECT_TRUE(r.kind == CanvasResolution::Kind::Main);
}
