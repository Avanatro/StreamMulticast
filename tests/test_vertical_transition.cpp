// Scene transition of the owned vertical canvas: pure decision (same type, settings and duration as main).
#include "TestRunner.hpp"
#include "core/VerticalTransition.hpp"

#include <obs-data.h>

#include <set>
#include <string>

using smulti::TransitionDesc;
using smulti::VerticalTransitionMode;
using smulti::VerticalTransitionPlan;
using smulti::VerticalTransitionSettings;

namespace {

smulti::FileExists exists_in(std::set<std::string> files)
{
	return [files](const std::string &p) { return files.count(p) > 0; };
}

const smulti::FileExists kNothingExists = [](const std::string &) { return false; };

/** Value of a string key in the plan's settings json. */
std::string plan_string(const VerticalTransitionPlan &plan, const char *key)
{
	obs_data_t *d = obs_data_create_from_json(plan.settings_json.c_str());
	if (!d)
		return "<no json>";
	const char *v = obs_data_get_string(d, key);
	std::string out = v ? v : "";
	obs_data_release(d);
	return out;
}

int64_t plan_int(const VerticalTransitionPlan &plan, const char *key)
{
	obs_data_t *d = obs_data_create_from_json(plan.settings_json.c_str());
	if (!d)
		return -1;
	const int64_t v = obs_data_get_int(d, key);
	obs_data_release(d);
	return v;
}

obs_data_t *stinger_settings(const char *path)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "path", path);
	obs_data_set_int(s, "transition_point", 640);
	obs_data_set_int(s, "tp_type", 0);
	return s;
}

} // namespace

SMULTI_TEST(vertical_transition_follow_copies_type_and_duration)
{
	obs_data_t *fade = obs_data_create();
	obs_data_set_int(fade, "some_key", 7);
	TransitionDesc main{"fade_transition", "Fade", fade, 450};

	VerticalTransitionSettings s; // follow by default
	EXPECT_TRUE(s.mode == VerticalTransitionMode::Follow);
	const VerticalTransitionPlan plan = smulti::decide_vertical_transition(s, main, nullptr, kNothingExists);
	EXPECT_EQ(plan.instant, false);
	EXPECT_EQ(plan.id, std::string("fade_transition"));
	EXPECT_EQ(plan.duration_ms, 450u);
	EXPECT_EQ(plan_int(plan, "some_key"), 7);
	EXPECT_EQ(plan.scale_aspect, false);
	/* the main settings are only read */
	EXPECT_EQ(obs_data_get_int(fade, "some_key"), 7);
	obs_data_release(fade);

	/* duration actually used by the main switch: scene override vs frontend duration */
	EXPECT_EQ(smulti::main_transition_duration_ms("Stinger", "Stinger", 1200, 300), 1200u);
	EXPECT_EQ(smulti::main_transition_duration_ms("Fade", "Stinger", 1200, 300), 300u);
	EXPECT_EQ(smulti::main_transition_duration_ms("Fade", "", 0, 300), 300u);
	EXPECT_EQ(smulti::main_transition_duration_ms("Stinger", "Stinger", 0, 500), 500u);
	EXPECT_EQ(smulti::main_transition_duration_ms("Fade", "", 0, -5), 0u);
}

SMULTI_TEST(vertical_transition_cut_is_instant)
{
	obs_data_t *fade = obs_data_create();
	TransitionDesc main{"fade_transition", "Fade", fade, 300};

	VerticalTransitionSettings cut;
	cut.mode = VerticalTransitionMode::Cut;
	EXPECT_EQ(smulti::decide_vertical_transition(cut, main, nullptr, kNothingExists).instant, true);

	/* follow a main cut -> instant as well */
	TransitionDesc main_cut{"cut_transition", "Cut", nullptr, 0};
	VerticalTransitionSettings follow;
	EXPECT_EQ(smulti::decide_vertical_transition(follow, main_cut, nullptr, kNothingExists).instant, true);

	/* nothing known about the main transition -> instant */
	TransitionDesc none{"", "", nullptr, 0};
	EXPECT_EQ(smulti::decide_vertical_transition(follow, none, nullptr, kNothingExists).instant, true);
	obs_data_release(fade);
}

SMULTI_TEST(vertical_transition_stinger_uses_vertical_file)
{
	obs_data_t *st = stinger_settings("E:/Stream/Avanatro/stinger.webm");
	TransitionDesc main{"obs_stinger_transition", "Stinger", st, 300};

	VerticalTransitionSettings s;
	s.stinger_path = "E:/Stream/Other/my-vertical.webm";
	const VerticalTransitionPlan plan = smulti::decide_vertical_transition(
		s, main, nullptr, exists_in({"E:/Stream/Other/my-vertical.webm", "E:/Stream/Avanatro/stinger-vertical.webm"}));
	EXPECT_EQ(plan.instant, false);
	EXPECT_EQ(plan.id, std::string("obs_stinger_transition"));
	EXPECT_EQ(plan_string(plan, "path"), std::string("E:/Stream/Other/my-vertical.webm"));
	EXPECT_EQ(plan_int(plan, "transition_point"), 640);
	EXPECT_EQ(plan.scale_aspect, false);
	/* the main stinger keeps its 16:9 file */
	EXPECT_EQ(std::string(obs_data_get_string(st, "path")), std::string("E:/Stream/Avanatro/stinger.webm"));

	/* a configured file that does not exist falls back to the auto rule */
	s.stinger_path = "E:/missing.webm";
	const VerticalTransitionPlan p2 = smulti::decide_vertical_transition(
		s, main, nullptr, exists_in({"E:/Stream/Avanatro/stinger-vertical.webm"}));
	EXPECT_EQ(plan_string(p2, "path"), std::string("E:/Stream/Avanatro/stinger-vertical.webm"));
	obs_data_release(st);
}

SMULTI_TEST(vertical_transition_stinger_autodetect_suffix)
{
	/* "<stem>-vertical.<ext>" wins over "stinger-vertical.<ext>" */
	EXPECT_EQ(smulti::vertical_stinger_file("E:\\Stream\\intro.mov", "",
						exists_in({"E:\\Stream\\intro-vertical.mov", "E:\\Stream\\stinger-vertical.mov"})),
		  std::string("E:\\Stream\\intro-vertical.mov"));
	EXPECT_EQ(smulti::vertical_stinger_file("E:\\Stream\\intro.mov", "", exists_in({"E:\\Stream\\stinger-vertical.mov"})),
		  std::string("E:\\Stream\\stinger-vertical.mov"));
	EXPECT_EQ(smulti::vertical_stinger_file("E:/Stream/Avanatro/stinger.webm", "",
						exists_in({"E:/Stream/Avanatro/stinger-vertical.webm"})),
		  std::string("E:/Stream/Avanatro/stinger-vertical.webm"));
	EXPECT_EQ(smulti::vertical_stinger_file("E:/a/b.c/clip", "", exists_in({"E:/a/b.c/clip-vertical"})),
		  std::string("E:/a/b.c/clip-vertical"));
	EXPECT_EQ(smulti::vertical_stinger_file("", "", kNothingExists), std::string(""));

	/* track matte: "<stem>-vertical" rule, only when track matte is enabled */
	obs_data_t *st = stinger_settings("E:/S/stinger.webm");
	obs_data_set_bool(st, "track_matte_enabled", true);
	obs_data_set_string(st, "track_matte_path", "E:/S/matte.webm");
	TransitionDesc main{"obs_stinger_transition", "Stinger", st, 300};
	VerticalTransitionSettings s;
	const auto files = exists_in({"E:/S/stinger-vertical.webm", "E:/S/matte-vertical.webm"});
	VerticalTransitionPlan plan = smulti::decide_vertical_transition(s, main, nullptr, files);
	EXPECT_EQ(plan_string(plan, "path"), std::string("E:/S/stinger-vertical.webm"));
	EXPECT_EQ(plan_string(plan, "track_matte_path"), std::string("E:/S/matte-vertical.webm"));

	obs_data_set_bool(st, "track_matte_enabled", false);
	plan = smulti::decide_vertical_transition(s, main, nullptr, files);
	EXPECT_EQ(plan_string(plan, "track_matte_path"), std::string("E:/S/matte.webm"));
	obs_data_release(st);
}

SMULTI_TEST(vertical_transition_stinger_fallback_aspect)
{
	obs_data_t *st = stinger_settings("E:/Stream/Avanatro/stinger.webm");
	TransitionDesc main{"obs_stinger_transition", "Stinger", st, 300};
	VerticalTransitionSettings s;
	const VerticalTransitionPlan plan = smulti::decide_vertical_transition(s, main, nullptr, kNothingExists);
	EXPECT_EQ(plan.instant, false);
	EXPECT_EQ(plan_string(plan, "path"), std::string("E:/Stream/Avanatro/stinger.webm"));
	EXPECT_EQ(plan.scale_aspect, true);
	EXPECT_EQ(plan_int(plan, "transition_point"), 640);
	obs_data_release(st);
}

SMULTI_TEST(vertical_transition_custom_mode)
{
	obs_data_t *st = stinger_settings("E:/S/stinger.webm");
	obs_data_t *fade = obs_data_create();
	obs_data_set_int(fade, "marker", 3);
	TransitionDesc main{"obs_stinger_transition", "Stinger", st, 300};
	TransitionDesc custom{"fade_transition", "Slow fade", fade, 900};

	VerticalTransitionSettings s;
	s.mode = VerticalTransitionMode::Custom;
	s.custom_name = "Slow fade";
	VerticalTransitionPlan plan = smulti::decide_vertical_transition(s, main, &custom, kNothingExists);
	EXPECT_EQ(plan.instant, false);
	EXPECT_EQ(plan.id, std::string("fade_transition"));
	EXPECT_EQ(plan.duration_ms, 900u);
	EXPECT_EQ(plan_int(plan, "marker"), 3);

	/* custom transition not found -> follow main */
	plan = smulti::decide_vertical_transition(s, main, nullptr, kNothingExists);
	EXPECT_EQ(plan.id, std::string("obs_stinger_transition"));

	/* custom cut -> instant */
	TransitionDesc cut{"cut_transition", "Cut", nullptr, 0};
	EXPECT_EQ(smulti::decide_vertical_transition(s, main, &cut, kNothingExists).instant, true);
	obs_data_release(fade);
	obs_data_release(st);
}

SMULTI_TEST(vertical_transition_mode_roundtrip)
{
	EXPECT_EQ(std::string(smulti::vertical_transition_mode_name(VerticalTransitionMode::Follow)), std::string("follow"));
	EXPECT_EQ(std::string(smulti::vertical_transition_mode_name(VerticalTransitionMode::Cut)), std::string("cut"));
	EXPECT_EQ(std::string(smulti::vertical_transition_mode_name(VerticalTransitionMode::Custom)), std::string("custom"));
	EXPECT_TRUE(smulti::parse_vertical_transition_mode("cut") == VerticalTransitionMode::Cut);
	EXPECT_TRUE(smulti::parse_vertical_transition_mode("custom") == VerticalTransitionMode::Custom);
	EXPECT_TRUE(smulti::parse_vertical_transition_mode("follow") == VerticalTransitionMode::Follow);
	EXPECT_TRUE(smulti::parse_vertical_transition_mode("") == VerticalTransitionMode::Follow);
	EXPECT_TRUE(smulti::parse_vertical_transition_mode("bogus") == VerticalTransitionMode::Follow);

	VerticalTransitionSettings s;
	s.mode = VerticalTransitionMode::Custom;
	s.custom_name = "Slow fade";
	s.stinger_path = "E:/S/v.webm";
	obs_data_t *data = obs_data_create();
	smulti::vertical_transition_save(data, s);
	EXPECT_EQ(std::string(obs_data_get_string(data, smulti::kVerticalTransitionModeKey)), std::string("custom"));
	const VerticalTransitionSettings back = smulti::vertical_transition_load(data);
	obs_data_release(data);
	EXPECT_TRUE(back.mode == VerticalTransitionMode::Custom);
	EXPECT_EQ(back.custom_name, std::string("Slow fade"));
	EXPECT_EQ(back.stinger_path, std::string("E:/S/v.webm"));

	/* missing keys -> defaults; the removed wait-for-stinger key is ignored */
	obs_data_t *old = obs_data_create();
	obs_data_set_bool(old, "streammulticast_vertical_link_wait_stinger", true);
	const VerticalTransitionSettings def = smulti::vertical_transition_load(old);
	obs_data_release(old);
	EXPECT_TRUE(def.mode == VerticalTransitionMode::Follow);
	EXPECT_TRUE(def.custom_name.empty());
	EXPECT_TRUE(def.stinger_path.empty());
}
