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

SMULTI_TEST(scene_link_stinger_delay_uses_transition_point)
{
	EXPECT_EQ(smulti::linked_switch_delay_ms("obs_stinger_transition", 0, 380, 60, 1), 380);
	/* frame based: 30 frames at 60 fps = 500 ms; at 30000/1001 fps = 1001 ms */
	EXPECT_EQ(smulti::linked_switch_delay_ms("obs_stinger_transition", 1, 30, 60, 1), 500);
	EXPECT_EQ(smulti::linked_switch_delay_ms("obs_stinger_transition", 1, 30, 30000, 1001), 1001);
	/* unusable input never delays, huge values are capped */
	EXPECT_EQ(smulti::linked_switch_delay_ms("obs_stinger_transition", 1, 30, 0, 1), 0);
	EXPECT_EQ(smulti::linked_switch_delay_ms("obs_stinger_transition", 0, -5, 60, 1), 0);
	EXPECT_EQ(smulti::linked_switch_delay_ms("obs_stinger_transition", 0, 999999, 60, 1), 120000);
}

SMULTI_TEST(scene_link_non_stinger_switches_immediately)
{
	EXPECT_EQ(smulti::linked_switch_delay_ms("fade_transition", 0, 380, 60, 1), 0);
	EXPECT_EQ(smulti::linked_switch_delay_ms("cut_transition", 0, 380, 60, 1), 0);
	EXPECT_EQ(smulti::linked_switch_delay_ms("", 0, 380, 60, 1), 0);
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

SMULTI_TEST(scene_link_switch_immediate_by_default)
{
	SceneLinkSettings s; // wait_for_stinger defaults to false
	EXPECT_EQ(s.wait_for_stinger, false);
	EXPECT_EQ(smulti::linked_switch_delay_ms(s, "obs_stinger_transition", 0, 380, 60, 1), 0);
	EXPECT_EQ(smulti::linked_switch_delay_ms(s, "fade_transition", 0, 380, 60, 1), 0);
}

SMULTI_TEST(scene_link_wait_for_stinger_keeps_delay)
{
	SceneLinkSettings s;
	s.wait_for_stinger = true;
	EXPECT_EQ(smulti::linked_switch_delay_ms(s, "obs_stinger_transition", 0, 380, 60, 1), 380);
	EXPECT_EQ(smulti::linked_switch_delay_ms(s, "fade_transition", 0, 380, 60, 1), 0);
}

SMULTI_TEST(scene_link_wait_for_stinger_persisted)
{
	SceneLinkSettings s;
	s.wait_for_stinger = true;
	obs_data_t *data = obs_data_create();
	smulti::scene_link_save(data, s);
	EXPECT_TRUE(obs_data_has_user_value(data, smulti::kSceneLinkWaitStingerKey));
	SceneLinkSettings back = smulti::scene_link_load(data);
	EXPECT_EQ(back.wait_for_stinger, true);
	obs_data_release(data);

	obs_data_t *empty = obs_data_create();
	EXPECT_EQ(smulti::scene_link_load(empty).wait_for_stinger, false);
	obs_data_release(empty);
}
