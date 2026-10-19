// Linked scene switching: main program scene -> vertical canvas scene (pure resolution).
#include "TestRunner.hpp"
#include "core/SceneLink.hpp"

#include <obs-data.h>

using smulti::SceneLinkSettings;

static bool is_keep(const std::optional<std::string> &r)
{
	return !r.has_value();
}

SMULTI_TEST(scene_link_default_prefers_v_prefix)
{
	SceneLinkSettings s;
	auto r = smulti::resolve_linked_scene("Gaming", s, {"V Gaming", "Gaming"});
	EXPECT_TRUE(r.has_value());
	EXPECT_EQ(r.value_or(""), "V Gaming");
}

SMULTI_TEST(scene_link_default_exact_name_fallback)
{
	SceneLinkSettings s;
	auto r = smulti::resolve_linked_scene("Gaming", s, {"Gaming"});
	EXPECT_EQ(r.value_or("<keep>"), "Gaming");
}

SMULTI_TEST(scene_link_no_match_keeps_current)
{
	SceneLinkSettings s;
	EXPECT_TRUE(is_keep(smulti::resolve_linked_scene("Starting Soon", s, {"V Gaming"})));
}

SMULTI_TEST(scene_link_explicit_overrides_default)
{
	SceneLinkSettings s;
	s.explicit_map["Gaming"] = "V BRB";
	auto r = smulti::resolve_linked_scene("Gaming", s, {"V Gaming", "V BRB"});
	EXPECT_EQ(r.value_or("<keep>"), "V BRB");
}

SMULTI_TEST(scene_link_explicit_keep)
{
	SceneLinkSettings s;
	s.explicit_map["Gaming"] = "";
	EXPECT_TRUE(is_keep(smulti::resolve_linked_scene("Gaming", s, {"V Gaming"})));
}

SMULTI_TEST(scene_link_explicit_missing_falls_back)
{
	SceneLinkSettings s;
	s.explicit_map["Gaming"] = "V Deleted";
	auto r = smulti::resolve_linked_scene("Gaming", s, {"V Gaming"});
	EXPECT_EQ(r.value_or("<keep>"), "V Gaming");
}

SMULTI_TEST(scene_link_disabled_never_switches)
{
	SceneLinkSettings s;
	s.enabled = false;
	s.explicit_map["Gaming"] = "V BRB";
	EXPECT_TRUE(is_keep(smulti::resolve_linked_scene("Gaming", s, {"V Gaming", "V BRB"})));
	EXPECT_TRUE(is_keep(smulti::resolve_linked_scene("Other", s, {"V Other"})));
}

SMULTI_TEST(scene_link_rename_migrates_key_and_value)
{
	SceneLinkSettings s;
	s.explicit_map["Gaming"] = "V BRB";
	s.explicit_map["Chat"] = "V Gaming";
	EXPECT_TRUE(smulti::rename_scene_in_links(s, "Gaming", "Games"));
	EXPECT_EQ(s.explicit_map.count("Gaming"), static_cast<size_t>(0));
	EXPECT_EQ(s.explicit_map["Games"], "V BRB");
	EXPECT_TRUE(smulti::rename_scene_in_links(s, "V BRB", "V Break"));
	EXPECT_EQ(s.explicit_map["Games"], "V Break");
	EXPECT_TRUE(!smulti::rename_scene_in_links(s, "Nothing", "Else"));
}

SMULTI_TEST(scene_link_save_load_roundtrip)
{
	SceneLinkSettings s;
	s.enabled = false;
	s.explicit_map["Gaming"] = "V BRB";
	s.explicit_map["BRB"] = "";

	obs_data_t *data = obs_data_create();
	smulti::scene_link_save(data, s);
	SceneLinkSettings back = smulti::scene_link_load(data);
	obs_data_release(data);

	EXPECT_EQ(back.enabled, false);
	EXPECT_TRUE(back.explicit_map == s.explicit_map);
}

SMULTI_TEST(scene_link_load_defaults_when_keys_missing)
{
	obs_data_t *data = obs_data_create();
	SceneLinkSettings back = smulti::scene_link_load(data);
	obs_data_release(data);
	EXPECT_EQ(back.enabled, true);
	EXPECT_TRUE(back.explicit_map.empty());
}
