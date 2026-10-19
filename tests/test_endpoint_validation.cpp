#include "TestRunner.hpp"
#include "core/EndpointValidation.hpp"

using smulti::DuplicateKind;
using smulti::Endpoint;
using smulti::StreamTarget;

static Endpoint make(const char *name, const char *server, const char *key, bool enabled = true)
{
	Endpoint ep = Endpoint::make_default(name);
	ep.server_url = server;
	ep.stream_key = key;
	ep.enabled = enabled;
	return ep;
}

/* ---- normalize_server_url ---- */

SMULTI_TEST(normalize_trims_and_lowercases_scheme_and_host)
{
	EXPECT_EQ(smulti::normalize_server_url("  RTMP://Live.Twitch.TV/app  "), "rtmp://live.twitch.tv/app");
}

SMULTI_TEST(normalize_strips_trailing_slashes)
{
	EXPECT_EQ(smulti::normalize_server_url("rtmp://host/app/"), "rtmp://host/app");
	EXPECT_EQ(smulti::normalize_server_url("rtmp://host/app///"), "rtmp://host/app");
	EXPECT_EQ(smulti::normalize_server_url("rtmp://host/"), "rtmp://host");
}

SMULTI_TEST(normalize_keeps_path_case)
{
	EXPECT_EQ(smulti::normalize_server_url("rtmp://HOST/App/Live"), "rtmp://host/App/Live");
}

SMULTI_TEST(normalize_keeps_port)
{
	EXPECT_EQ(smulti::normalize_server_url("rtmp://HOST:1935/app"), "rtmp://host:1935/app");
}

SMULTI_TEST(normalize_without_scheme_lowercases_host)
{
	EXPECT_EQ(smulti::normalize_server_url("Host.Example.com/App/"), "host.example.com/App");
}

SMULTI_TEST(normalize_blank_gives_empty)
{
	EXPECT_EQ(smulti::normalize_server_url(""), "");
	EXPECT_EQ(smulti::normalize_server_url("   "), "");
}

/* ---- is_duplicate_target ---- */

SMULTI_TEST(duplicate_when_normalized_server_and_key_equal)
{
	StreamTarget a{"rtmp://Live.Example.com/app/", "key123"};
	StreamTarget b{" rtmp://live.example.com/app", "key123 "};
	EXPECT_TRUE(smulti::is_duplicate_target(a, b));
}

SMULTI_TEST(not_duplicate_when_key_differs)
{
	StreamTarget a{"rtmp://host/app", "key1"};
	StreamTarget b{"rtmp://host/app", "key2"};
	EXPECT_TRUE(!smulti::is_duplicate_target(a, b));
}

SMULTI_TEST(key_comparison_is_case_sensitive)
{
	StreamTarget a{"rtmp://host/app", "AbC"};
	StreamTarget b{"rtmp://host/app", "abc"};
	EXPECT_TRUE(!smulti::is_duplicate_target(a, b));
}

SMULTI_TEST(not_duplicate_when_server_differs)
{
	StreamTarget a{"rtmp://host-a/app", "key"};
	StreamTarget b{"rtmp://host-b/app", "key"};
	EXPECT_TRUE(!smulti::is_duplicate_target(a, b));
}

SMULTI_TEST(not_duplicate_when_path_case_differs)
{
	StreamTarget a{"rtmp://host/App", "key"};
	StreamTarget b{"rtmp://host/app", "key"};
	EXPECT_TRUE(!smulti::is_duplicate_target(a, b));
}

SMULTI_TEST(empty_keys_are_never_duplicates)
{
	StreamTarget a{"rtmp://host/app", ""};
	StreamTarget b{"rtmp://host/app", "  "};
	EXPECT_TRUE(!smulti::is_duplicate_target(a, a));
	EXPECT_TRUE(!smulti::is_duplicate_target(a, b));
}

/* ---- find_duplicate ---- */

SMULTI_TEST(find_duplicate_detects_other_enabled_endpoint)
{
	Endpoint a = make("A", "rtmp://host/app", "k");
	Endpoint b = make("B", "RTMP://HOST/app/", "k");
	auto r = smulti::find_duplicate(b, {a, b}, std::nullopt);
	EXPECT_TRUE(r.kind == DuplicateKind::OtherEndpoint);
	EXPECT_EQ(r.other_name, "A");
}

SMULTI_TEST(find_duplicate_ignores_disabled_and_self)
{
	Endpoint a = make("A", "rtmp://host/app", "k", /*enabled=*/false);
	Endpoint b = make("B", "rtmp://host/app", "k");
	auto r = smulti::find_duplicate(b, {a, b}, std::nullopt);
	EXPECT_TRUE(r.kind == DuplicateKind::None);
}

SMULTI_TEST(find_duplicate_detects_main_service)
{
	Endpoint b = make("B", "rtmp://host/app", "k");
	StreamTarget main_t{"rtmp://HOST/app/", "k"};
	auto r = smulti::find_duplicate(b, {b}, main_t);
	EXPECT_TRUE(r.kind == DuplicateKind::MainService);
}

SMULTI_TEST(find_duplicate_main_service_with_other_key_is_fine)
{
	Endpoint b = make("B", "rtmp://host/app", "k");
	StreamTarget main_t{"rtmp://host/app", "other"};
	EXPECT_TRUE(smulti::find_duplicate(b, {b}, main_t).kind == DuplicateKind::None);
}
