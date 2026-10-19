// Canvas reference on Endpoint and migration of the removed framing modes.
// Pure obs_data only: no obs_startup, no frontend, no Qt.
#include "TestRunner.hpp"
#include "core/CanvasMigration.hpp"
#include "core/Endpoint.hpp"

#include <obs-data.h>

using smulti::Endpoint;
using smulti::OutputOrientation;

SMULTI_TEST(canvas_endpoint_roundtrip_preserves_canvas_ref)
{
	Endpoint ep = Endpoint::make_default("Vertical Out");
	ep.canvas_uuid = "abc-123";
	ep.canvas_name = "My Canvas";

	obs_data_t *data = ep.serialize();
	Endpoint out = Endpoint::deserialize(data);
	obs_data_release(data);

	EXPECT_EQ(out.canvas_uuid, std::string("abc-123"));
	EXPECT_EQ(out.canvas_name, std::string("My Canvas"));
}

SMULTI_TEST(canvas_old_config_without_key_resolves_to_main)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "id", "id-1");
	obs_data_set_string(data, "name", "Legacy");
	obs_data_set_string(data, "server_url", "rtmp://live.example.com/app");
	obs_data_set_string(data, "stream_key", "k");

	Endpoint out = Endpoint::deserialize(data);
	obs_data_release(data);

	EXPECT_EQ(out.canvas_uuid, std::string(""));
	EXPECT_EQ(out.canvas_name, std::string(""));
}

SMULTI_TEST(canvas_migrate_letterbox_to_vertical_canvas)
{
	Endpoint ep = Endpoint::make_default("Letterbox");
	ep.orientation = OutputOrientation::Vertical1080x1920Letterbox;

	EXPECT_TRUE(smulti::migrate_legacy_orientation(ep));
	EXPECT_EQ(ep.canvas_name, std::string("StreamMulticast Vertical"));
	EXPECT_EQ(ep.canvas_uuid, std::string(""));
	EXPECT_TRUE(ep.orientation == OutputOrientation::SourceMatch);
}

SMULTI_TEST(canvas_migrate_centercrop_to_vertical_canvas)
{
	Endpoint ep = Endpoint::make_default("Crop");
	ep.orientation = OutputOrientation::Vertical1080x1920CenterCrop;

	EXPECT_TRUE(smulti::migrate_legacy_orientation(ep));
	EXPECT_EQ(ep.canvas_name, std::string("StreamMulticast Vertical"));
	EXPECT_EQ(ep.canvas_uuid, std::string(""));
	EXPECT_TRUE(ep.orientation == OutputOrientation::SourceMatch);
}

SMULTI_TEST(canvas_migrate_source_match_untouched)
{
	Endpoint ep = Endpoint::make_default("Plain");
	ep.canvas_name = "X";
	ep.canvas_uuid = "u-1";

	EXPECT_TRUE(!smulti::migrate_legacy_orientation(ep));
	EXPECT_EQ(ep.canvas_name, std::string("X"));
	EXPECT_EQ(ep.canvas_uuid, std::string("u-1"));
	EXPECT_TRUE(ep.orientation == OutputOrientation::SourceMatch);
}

SMULTI_TEST(canvas_migrate_is_idempotent)
{
	Endpoint ep = Endpoint::make_default("Twice");
	ep.orientation = OutputOrientation::Vertical1080x1920Letterbox;

	EXPECT_TRUE(smulti::migrate_legacy_orientation(ep));
	EXPECT_TRUE(!smulti::migrate_legacy_orientation(ep));
	EXPECT_EQ(ep.canvas_name, std::string("StreamMulticast Vertical"));
}
