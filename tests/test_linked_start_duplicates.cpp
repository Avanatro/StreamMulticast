// Linked start must skip endpoints that duplicate the main service or an
// earlier selected endpoint (same normalized server + key).
#include "TestRunner.hpp"
#include "core/LinkedStart.hpp"

using smulti::DuplicateKind;
using smulti::Endpoint;
using smulti::StreamTarget;

static Endpoint make(const char *name, const char *server, const char *key, bool enabled = true, bool linked = true)
{
	Endpoint ep = Endpoint::make_default(name);
	ep.server_url = server;
	ep.stream_key = key;
	ep.enabled = enabled;
	ep.linked_to_main = linked;
	return ep;
}

SMULTI_TEST(plan_skips_endpoint_equal_to_main_service)
{
	std::vector<Endpoint> all = {make("same-as-main", "rtmp://host/app", "k"), make("other", "rtmp://x/app", "k2")};
	StreamTarget main_t{"RTMP://Host/app/", "k"};
	auto plan = smulti::plan_linked_start(all, main_t);
	EXPECT_EQ(plan.to_start.size(), static_cast<size_t>(1));
	EXPECT_EQ(plan.skipped_duplicates.size(), static_cast<size_t>(1));
	if (plan.to_start.size() == 1 && plan.skipped_duplicates.size() == 1) {
		EXPECT_EQ(plan.to_start[0].name, "other");
		EXPECT_EQ(plan.skipped_duplicates[0].name, "same-as-main");
		EXPECT_TRUE(plan.skipped_duplicates[0].reason == DuplicateKind::MainService);
	}
}

SMULTI_TEST(plan_first_of_two_duplicate_endpoints_wins)
{
	std::vector<Endpoint> all = {make("first", "rtmp://host/app", "k"), make("second", "rtmp://HOST/app/", "k")};
	auto plan = smulti::plan_linked_start(all, std::nullopt);
	EXPECT_EQ(plan.to_start.size(), static_cast<size_t>(1));
	EXPECT_EQ(plan.skipped_duplicates.size(), static_cast<size_t>(1));
	if (plan.to_start.size() == 1 && plan.skipped_duplicates.size() == 1) {
		EXPECT_EQ(plan.to_start[0].name, "first");
		EXPECT_EQ(plan.skipped_duplicates[0].name, "second");
		EXPECT_TRUE(plan.skipped_duplicates[0].reason == DuplicateKind::OtherEndpoint);
	}
}

SMULTI_TEST(plan_without_duplicates_starts_everything_selected)
{
	std::vector<Endpoint> all = {make("a", "rtmp://a/app", "k"), make("b", "rtmp://b/app", "k"),
				     make("off", "rtmp://c/app", "k", false)};
	auto plan = smulti::plan_linked_start(all, StreamTarget{"rtmp://m/app", "k"});
	EXPECT_EQ(plan.to_start.size(), static_cast<size_t>(2));
	EXPECT_EQ(plan.skipped_duplicates.size(), static_cast<size_t>(0));
}

SMULTI_TEST(plan_duplicate_of_disabled_endpoint_still_starts)
{
	std::vector<Endpoint> all = {make("off", "rtmp://host/app", "k", false), make("on", "rtmp://host/app", "k")};
	auto plan = smulti::plan_linked_start(all, std::nullopt);
	EXPECT_EQ(plan.to_start.size(), static_cast<size_t>(1));
	EXPECT_EQ(plan.skipped_duplicates.size(), static_cast<size_t>(0));
}
