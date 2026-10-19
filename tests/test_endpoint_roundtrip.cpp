// Harness smoke test: exercises Endpoint serialization using only obs_data
// (libobs util), no obs_startup, no frontend, no Qt.
#include "TestRunner.hpp"
#include "core/Endpoint.hpp"

#include <obs-data.h>

using smulti::Endpoint;

SMULTI_TEST(endpoint_roundtrip_preserves_all_fields)
{
	Endpoint ep = Endpoint::make_default("Twitch Main");
	ep.server_url = "rtmp://live.example.com/app";
	ep.stream_key = "k";
	ep.video_bitrate_kbps = 4500;
	ep.enabled = false;
	ep.linked_to_main = false;
	ep.sort_order = 3;

	obs_data_t *data = ep.serialize();
	Endpoint out = Endpoint::deserialize(data);
	obs_data_release(data);

	EXPECT_EQ(out.id, ep.id);
	EXPECT_EQ(out.name, ep.name);
	EXPECT_EQ(out.server_url, ep.server_url);
	EXPECT_EQ(out.stream_key, ep.stream_key);
	EXPECT_EQ(out.video_bitrate_kbps, 4500);
	EXPECT_EQ(out.enabled, false);
	EXPECT_EQ(out.linked_to_main, false);
	EXPECT_EQ(out.sort_order, 3);
}
