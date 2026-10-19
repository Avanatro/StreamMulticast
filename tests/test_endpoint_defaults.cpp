// A key that is absent from persisted data must deserialize to the documented
// make_default() value, not to the obs_data zero value (false / 0 / "").
#include "TestRunner.hpp"
#include "core/Endpoint.hpp"

#include <obs-data.h>

using smulti::Endpoint;

SMULTI_TEST(endpoint_missing_bools_deserialize_to_defaults)
{
	Endpoint def = Endpoint::make_default("x");
	obs_data_t *data = def.serialize();
	obs_data_erase(data, "enabled");
	obs_data_erase(data, "linked_to_main");

	Endpoint out = Endpoint::deserialize(data);
	obs_data_release(data);

	EXPECT_EQ(out.enabled, def.enabled);
	EXPECT_EQ(out.linked_to_main, def.linked_to_main);
}

SMULTI_TEST(endpoint_missing_numbers_deserialize_to_defaults)
{
	Endpoint def = Endpoint::make_default("x");
	obs_data_t *data = def.serialize();
	obs_data_erase(data, "keyframe_interval");
	obs_data_erase(data, "video_bitrate");
	obs_data_erase(data, "audio_bitrate");

	Endpoint out = Endpoint::deserialize(data);
	obs_data_release(data);

	EXPECT_EQ(out.keyframe_interval_sec, def.keyframe_interval_sec);
	EXPECT_EQ(out.video_bitrate_kbps, def.video_bitrate_kbps);
	EXPECT_EQ(out.audio_bitrate_kbps, def.audio_bitrate_kbps);
}

SMULTI_TEST(endpoint_present_false_is_kept_not_overridden_by_default)
{
	Endpoint ep = Endpoint::make_default("x");
	ep.enabled = false;
	ep.linked_to_main = false;
	obs_data_t *data = ep.serialize();
	Endpoint out = Endpoint::deserialize(data);
	obs_data_release(data);

	EXPECT_EQ(out.enabled, false);
	EXPECT_EQ(out.linked_to_main, false);
}
