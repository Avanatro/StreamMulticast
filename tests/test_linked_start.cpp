// Main-stream start must only start endpoints that are enabled AND linked.
#include "TestRunner.hpp"
#include "core/LinkedStart.hpp"

using smulti::Endpoint;

static Endpoint make(const char *name, bool enabled, bool linked)
{
	Endpoint ep = Endpoint::make_default(name);
	ep.enabled = enabled;
	ep.linked_to_main = linked;
	return ep;
}

SMULTI_TEST(linked_start_skips_disabled_endpoints)
{
	std::vector<Endpoint> all = {
		make("on-linked", true, true),
		make("off-linked", false, true),
	};
	auto sel = smulti::select_linked_for_start(all);
	EXPECT_EQ(sel.size(), static_cast<size_t>(1));
	if (!sel.empty())
		EXPECT_EQ(sel[0].name, "on-linked");
}

SMULTI_TEST(linked_start_skips_unlinked_endpoints)
{
	std::vector<Endpoint> all = {
		make("on-unlinked", true, false),
		make("off-unlinked", false, false),
		make("on-linked", true, true),
	};
	auto sel = smulti::select_linked_for_start(all);
	EXPECT_EQ(sel.size(), static_cast<size_t>(1));
	if (!sel.empty())
		EXPECT_EQ(sel[0].name, "on-linked");
}

SMULTI_TEST(linked_start_preserves_order)
{
	std::vector<Endpoint> all = {make("a", true, true), make("b", true, true), make("c", true, true)};
	auto sel = smulti::select_linked_for_start(all);
	EXPECT_EQ(sel.size(), static_cast<size_t>(3));
	if (sel.size() == 3) {
		EXPECT_EQ(sel[0].name, "a");
		EXPECT_EQ(sel[1].name, "b");
		EXPECT_EQ(sel[2].name, "c");
	}
}

SMULTI_TEST(linked_start_empty_input_gives_empty_selection)
{
	EXPECT_EQ(smulti::select_linked_for_start({}).size(), static_cast<size_t>(0));
}
