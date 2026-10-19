// Canvas reference on Endpoint and migration of the removed framing modes.
// Pure obs_data only: no obs_startup, no frontend, no Qt.
#include "TestRunner.hpp"
#include "core/CanvasMigration.hpp"
#include "core/Endpoint.hpp"

#include <obs-data.h>

#include <vector>

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

// A load that migrated anything must be written back; otherwise the old schema
// stays on disk until a later save that may never run (OBS can crash on exit
// before obs_module_unload).
SMULTI_TEST(canvas_migrate_marks_config_dirty)
{
	std::vector<Endpoint> eps;
	eps.push_back(Endpoint::make_default("Tiktok"));
	eps.back().orientation = OutputOrientation::Vertical1080x1920Letterbox;
	eps.push_back(Endpoint::make_default("Plain"));

	const auto res = smulti::migrate_config(1, 2, eps);

	EXPECT_TRUE(res.needs_save);
	EXPECT_EQ(res.migrated.size(), static_cast<size_t>(1));
	EXPECT_EQ(res.migrated[0].name, std::string("Tiktok"));
	EXPECT_EQ(res.migrated[0].old_orientation, static_cast<int>(OutputOrientation::Vertical1080x1920Letterbox));
	EXPECT_TRUE(eps[0].orientation == OutputOrientation::SourceMatch);
	EXPECT_EQ(eps[0].canvas_name, std::string("StreamMulticast Vertical"));
}

SMULTI_TEST(canvas_migrate_old_schema_without_changes_still_needs_save)
{
	std::vector<Endpoint> eps;
	eps.push_back(Endpoint::make_default("Plain"));

	const auto res = smulti::migrate_config(1, 2, eps);

	EXPECT_TRUE(res.needs_save);
	EXPECT_EQ(res.migrated.size(), static_cast<size_t>(0));
}

SMULTI_TEST(canvas_migrate_current_schema_stays_clean)
{
	std::vector<Endpoint> eps;
	eps.push_back(Endpoint::make_default("Plain"));

	const auto res = smulti::migrate_config(2, 2, eps);

	EXPECT_TRUE(!res.needs_save);
	EXPECT_EQ(res.migrated.size(), static_cast<size_t>(0));
}
