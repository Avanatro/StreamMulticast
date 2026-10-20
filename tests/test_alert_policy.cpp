// Live health alerts: pure rule (raise / hold / clear, edge-triggered), event payload, settings. obs_data only.
#include "TestRunner.hpp"
#include "core/AlertPolicy.hpp"

#include <obs-data.h>

#include <set>
#include <string>
#include <vector>

using smulti::AlertInput;
using smulti::AlertLevel;
using smulti::AlertPolicy;
using smulti::AlertSettings;
using smulti::AlertTransition;
using smulti::OutputState;

namespace {

/* One endpoint at 2 Hz (500 ms ticks), 30 fps = 15 frames per tick, target 3000 kbps. */
struct Sim {
	AlertPolicy policy;
	int64_t     t       = 0;
	uint64_t    dropped = 0;
	uint64_t    frames  = 0;
	OutputState state   = OutputState::Live;
	int         target  = 3000;
	double      actual  = 3000.0;
	std::vector<AlertTransition> all;

	std::vector<AlertTransition> tick(uint64_t drops = 0, uint64_t new_frames = 15)
	{
		t += 500;
		dropped += drops;
		frames += new_frames;
		AlertInput in;
		in.endpoint_id   = "ep1";
		in.endpoint_name = "Tiktok";
		in.state         = state;
		in.target_kbps   = target;
		in.actual_kbps   = actual;
		in.dropped_total = dropped;
		in.frames_total  = frames;
		in.now_ms        = t;
		auto out = policy.update(in);
		all.insert(all.end(), out.begin(), out.end());
		return out;
	}

	/* Runs ticks until the clock reaches `until_ms`; returns all transitions seen. */
	std::vector<AlertTransition> run_until(int64_t until_ms, uint64_t drops = 0, uint64_t new_frames = 15)
	{
		std::vector<AlertTransition> seen;
		while (t < until_ms) {
			auto out = tick(drops, new_frames);
			seen.insert(seen.end(), out.begin(), out.end());
		}
		return seen;
	}
};

int count_raised(const std::vector<AlertTransition> &v, const std::string &reason)
{
	int n = 0;
	for (const auto &x : v)
		if (x.raised && x.reason == reason)
			++n;
	return n;
}

int count_cleared(const std::vector<AlertTransition> &v, const std::string &reason)
{
	int n = 0;
	for (const auto &x : v)
		if (!x.raised && x.reason == reason)
			++n;
	return n;
}

} // namespace

SMULTI_TEST(alert_policy_reconnecting_raises_warning)
{
	Sim s;
	EXPECT_EQ(s.run_until(5000).size(), static_cast<size_t>(0));
	s.state = OutputState::Reconnecting;
	auto out = s.tick();
	EXPECT_EQ(out.size(), static_cast<size_t>(1));
	if (out.size() == 1) {
		EXPECT_TRUE(out[0].raised);
		EXPECT_TRUE(out[0].level == AlertLevel::Warning);
		EXPECT_EQ(out[0].reason, "reconnecting");
		EXPECT_EQ(out[0].endpoint_id, "ep1");
		EXPECT_EQ(out[0].endpoint_name, "Tiktok");
		EXPECT_EQ(std::string(smulti::alert_state_name(out[0].state)), "reconnecting");
	}
	EXPECT_TRUE(s.policy.status("ep1").level == AlertLevel::Warning);
}

SMULTI_TEST(alert_policy_failed_hard_raises_critical)
{
	Sim s;
	s.run_until(5000);
	s.state = OutputState::FailedHard;
	auto out = s.tick();
	EXPECT_EQ(out.size(), static_cast<size_t>(1));
	if (out.size() == 1) {
		EXPECT_TRUE(out[0].raised);
		EXPECT_TRUE(out[0].level == AlertLevel::Critical);
		EXPECT_EQ(out[0].reason, "failed");
	}
	EXPECT_TRUE(s.policy.status("ep1").level == AlertLevel::Critical);
	EXPECT_TRUE(s.policy.max_level() == AlertLevel::Critical);
	/* Reconnecting that escalates to FailedHard replaces the warning. */
	Sim r;
	r.run_until(5000);
	r.state = OutputState::Reconnecting;
	r.tick();
	r.state = OutputState::FailedHard;
	r.tick();
	EXPECT_EQ(count_raised(r.all, "reconnecting"), 1);
	EXPECT_EQ(count_cleared(r.all, "reconnecting"), 1);
	EXPECT_EQ(count_raised(r.all, "failed"), 1);
	EXPECT_TRUE(r.policy.status("ep1").level == AlertLevel::Critical);
}

SMULTI_TEST(alert_policy_dropped_frames_over_threshold_raises)
{
	Sim s;
	EXPECT_EQ(s.run_until(25000).size(), static_cast<size_t>(0)); // past the grace period, clean
	/* 1 dropped per 15 frames = 6.7 %: raised once the window holds >= 10 drops (10 ticks = 5 s). */
	s.run_until(35000, 1);
	EXPECT_EQ(count_raised(s.all, "dropped_frames"), 1);
	EXPECT_EQ(s.all.size(), static_cast<size_t>(1));
	EXPECT_TRUE(s.policy.status("ep1").level == AlertLevel::Warning);
	auto st = s.policy.status("ep1");
	EXPECT_EQ(st.reasons.size(), static_cast<size_t>(1));
	if (st.reasons.size() == 1)
		EXPECT_EQ(st.reasons[0], "dropped_frames");
}

SMULTI_TEST(alert_policy_dropped_frames_below_threshold_silent)
{
	/* Ratio below 2 %: 10 drops in every 10 s window (the minimum) but over 3000 frames = 0.33 %. */
	Sim a;
	for (int i = 0; i < 240; ++i)
		a.tick(i % 2 == 0 ? 1 : 0, 150);
	EXPECT_EQ(a.all.size(), static_cast<size_t>(0));
	/* Ratio above 2 % but fewer than 10 drops in the window: 9 drops in 10 s of 300 frames = 3 %. */
	Sim b;
	b.run_until(20000);
	for (int i = 0; i < 20; ++i)
		b.tick(i < 9 ? 1 : 0);
	EXPECT_EQ(b.all.size(), static_cast<size_t>(0));
}

SMULTI_TEST(alert_policy_low_bitrate_after_15s_raises)
{
	Sim s;
	s.run_until(20000);
	s.actual = 1000.0; // < 50 % of 3000
	/* Low since t=20500 (first tick below): not raised until 15 s later. */
	EXPECT_EQ(s.run_until(35000 - 500).size(), static_cast<size_t>(0));
	auto out = s.tick(); // t = 35000, 14.5 s after the first low tick
	EXPECT_EQ(out.size(), static_cast<size_t>(0));
	out = s.tick(); // t = 35500, 15.0 s after
	EXPECT_EQ(out.size(), static_cast<size_t>(1));
	if (out.size() == 1) {
		EXPECT_TRUE(out[0].raised);
		EXPECT_TRUE(out[0].level == AlertLevel::Warning);
		EXPECT_EQ(out[0].reason, "low_bitrate");
	}
}

SMULTI_TEST(alert_policy_short_bitrate_dip_silent)
{
	Sim s;
	s.run_until(20000);
	for (int round = 0; round < 3; ++round) {
		s.actual = 500.0;
		s.run_until(s.t + 14000); // 14 s low
		s.actual = 3000.0;
		s.run_until(s.t + 5000);
	}
	EXPECT_EQ(s.all.size(), static_cast<size_t>(0));
	/* Exactly 50 % is not below 50 %. */
	Sim e;
	e.run_until(20000);
	e.actual = 1500.0;
	e.run_until(e.t + 60000);
	EXPECT_EQ(e.all.size(), static_cast<size_t>(0));
}

SMULTI_TEST(alert_policy_startup_grace)
{
	/* Heavy drops during the first 10 s of Live, clean afterwards: never raised. */
	Sim d;
	d.run_until(10000, 7);
	d.run_until(60000);
	EXPECT_EQ(d.all.size(), static_cast<size_t>(0));
	/* Low bitrate from the very start: the 15 s hold only starts after the 10 s grace. */
	Sim b;
	b.actual = 200.0;
	EXPECT_EQ(b.run_until(25000).size(), static_cast<size_t>(0));
	auto out = b.tick(); // t = 25500: first checked tick 10500 + 15 s
	EXPECT_EQ(count_raised(out, "low_bitrate"), 1);
	/* A reconnect restarts the grace: low bitrate right after Live returns is not counted. */
	Sim r;
	r.run_until(30000);
	r.state = OutputState::Reconnecting;
	r.run_until(35000);
	r.state = OutputState::Live;
	r.actual = 100.0;
	auto seen = r.run_until(35000 + 24000);
	EXPECT_EQ(count_raised(seen, "low_bitrate"), 0);
}

SMULTI_TEST(alert_policy_hysteresis_clears_after_hold)
{
	/* Low bitrate: raised, recovered, cleared only after 10 s of continuous good bitrate. */
	Sim s;
	s.run_until(10000);
	s.actual = 1000.0;
	s.run_until(40000);
	EXPECT_EQ(count_raised(s.all, "low_bitrate"), 1);
	s.actual = 3000.0;
	s.run_until(s.t + 9500); // good since the first of these ticks: hold is 9.0 s
	EXPECT_EQ(count_cleared(s.all, "low_bitrate"), 0);
	/* A relapse resets the hold. */
	s.actual = 1000.0;
	s.run_until(s.t + 2000);
	s.actual = 3000.0;
	s.run_until(s.t + 9500);
	EXPECT_EQ(count_cleared(s.all, "low_bitrate"), 0);
	s.run_until(s.t + 1500);
	EXPECT_EQ(count_cleared(s.all, "low_bitrate"), 1);
	EXPECT_TRUE(s.policy.status("ep1").level == AlertLevel::None);

	/* State alert: Reconnecting clears 10 s after Live is back, not at once. */
	Sim r;
	r.run_until(5000);
	r.state = OutputState::Reconnecting;
	r.run_until(8000);
	r.state = OutputState::Live;
	r.run_until(r.t + 9000);
	EXPECT_EQ(count_cleared(r.all, "reconnecting"), 0);
	r.run_until(r.t + 1500);
	EXPECT_EQ(count_cleared(r.all, "reconnecting"), 1);

	/* Idle (user stop) clears at once. */
	Sim i;
	i.run_until(5000);
	i.state = OutputState::FailedHard;
	i.tick();
	i.state = OutputState::Idle;
	auto out = i.tick();
	EXPECT_EQ(count_cleared(out, "failed"), 1);
	EXPECT_TRUE(i.policy.status("ep1").level == AlertLevel::None);
}

SMULTI_TEST(alert_policy_raise_is_edge_triggered)
{
	Sim s;
	s.run_until(10000);
	s.actual = 100.0;
	s.run_until(120000); // a standing condition for minutes
	EXPECT_EQ(count_raised(s.all, "low_bitrate"), 1);
	EXPECT_EQ(s.all.size(), static_cast<size_t>(1));
	/* Same for a standing Reconnecting. */
	Sim r;
	r.run_until(3000);
	r.state = OutputState::Reconnecting;
	r.run_until(120000);
	EXPECT_EQ(r.all.size(), static_cast<size_t>(1));
	/* After a clear the next occurrence raises at once. */
	Sim q;
	q.run_until(5000);
	q.state = OutputState::Reconnecting;
	q.tick();
	q.state = OutputState::Idle;
	q.tick();
	q.state = OutputState::Reconnecting;
	auto out = q.tick();
	EXPECT_EQ(count_raised(out, "reconnecting"), 1);
	EXPECT_EQ(count_raised(q.all, "reconnecting"), 2);
}

SMULTI_TEST(alert_policy_counter_reset_no_negative)
{
	Sim s;
	s.dropped = 900;
	s.frames  = 90000;
	s.run_until(30000);
	/* New session: both counters restart small while the state stays Live. */
	s.dropped = 0;
	s.frames  = 0;
	s.tick(0, 20);
	s.tick(0, 20);
	s.run_until(s.t + 20000);
	EXPECT_EQ(s.all.size(), static_cast<size_t>(0));
	/* Only dropped_total resets (smaller than before): no unsigned wrap-around alert. */
	Sim d;
	d.dropped = 50;
	d.run_until(30000);
	d.dropped = 3;
	d.tick();
	d.run_until(d.t + 20000);
	EXPECT_EQ(d.all.size(), static_cast<size_t>(0));
	/* The window really restarted: real drops after the reset still raise. */
	Sim e;
	e.dropped = 500;
	e.frames  = 50000;
	e.run_until(30000);
	e.dropped = 0;
	e.frames  = 0;
	e.run_until(e.t + 20000, 1);
	EXPECT_EQ(count_raised(e.all, "dropped_frames"), 1);
}

SMULTI_TEST(alert_event_payload_has_no_secrets)
{
	AlertTransition t;
	t.raised        = true;
	t.level         = AlertLevel::Critical;
	t.reason        = "failed";
	t.endpoint_id   = "1b4e28ba-2fa1-11d2-883f-0016d3cca427";
	t.endpoint_name = "Tiktok";
	t.state         = OutputState::FailedHard;
	obs_data_t *p = smulti::alert_event_payload(t);
	EXPECT_TRUE(p != nullptr);
	if (!p)
		return;
	EXPECT_EQ(std::string(obs_data_get_string(p, "endpoint_id")), t.endpoint_id);
	EXPECT_EQ(std::string(obs_data_get_string(p, "endpoint_name")), "Tiktok");
	EXPECT_EQ(std::string(obs_data_get_string(p, "level")), "critical");
	EXPECT_EQ(std::string(obs_data_get_string(p, "reason")), "failed");
	EXPECT_TRUE(obs_data_get_bool(p, "active"));
	EXPECT_EQ(std::string(obs_data_get_string(p, "state")), "failed");

	/* Exactly these six fields; nothing that could carry a URL, key, token or error text. */
	std::set<std::string> names;
	for (obs_data_item_t *it = obs_data_first(p); it; obs_data_item_next(&it))
		names.insert(obs_data_item_get_name(it));
	const std::set<std::string> expected = {"endpoint_id", "endpoint_name", "level", "reason", "active", "state"};
	EXPECT_TRUE(names == expected);
	const std::string json = obs_data_get_json(p);
	EXPECT_TRUE(json.find("://") == std::string::npos);
	EXPECT_TRUE(json.find("last_error") == std::string::npos);
	EXPECT_TRUE(json.find("key") == std::string::npos);

	/* A cleared transition carries active=false and the warning level. */
	t.raised = false;
	t.level  = AlertLevel::Warning;
	t.reason = "low_bitrate";
	t.state  = OutputState::Live;
	obs_data_t *c = smulti::alert_event_payload(t);
	EXPECT_TRUE(c != nullptr);
	if (c) {
		EXPECT_TRUE(!obs_data_get_bool(c, "active"));
		EXPECT_EQ(std::string(obs_data_get_string(c, "level")), "warning");
		EXPECT_EQ(std::string(obs_data_get_string(c, "state")), "live");
		obs_data_release(c);
	}
	obs_data_release(p);
}

SMULTI_TEST(alert_settings_roundtrip)
{
	/* Defaults: sound off, websocket on; a missing file or missing keys keep them. */
	AlertSettings def;
	EXPECT_TRUE(!def.sound_enabled);
	EXPECT_TRUE(def.websocket_enabled);
	AlertSettings none = smulti::alert_settings_load(nullptr);
	EXPECT_TRUE(!none.sound_enabled);
	EXPECT_TRUE(none.websocket_enabled);
	obs_data_t *empty = obs_data_create();
	AlertSettings from_empty = smulti::alert_settings_load(empty);
	EXPECT_TRUE(!from_empty.sound_enabled);
	EXPECT_TRUE(from_empty.websocket_enabled);
	obs_data_release(empty);

	AlertSettings in;
	in.sound_enabled     = true;
	in.websocket_enabled = false;
	obs_data_t *data = obs_data_create();
	smulti::alert_settings_save(data, in);
	/* through JSON, like the settings file */
	obs_data_t *reread = obs_data_create_from_json(obs_data_get_json(data));
	AlertSettings out = smulti::alert_settings_load(reread);
	EXPECT_TRUE(out.sound_enabled);
	EXPECT_TRUE(!out.websocket_enabled);
	obs_data_release(reread);
	obs_data_release(data);
}

SMULTI_TEST(sound_throttle_once_per_30s)
{
	int64_t last = -1;
	EXPECT_TRUE(smulti::alert_sound_allowed(1000, last));
	EXPECT_TRUE(!smulti::alert_sound_allowed(1000 + 29999, last));
	EXPECT_TRUE(smulti::alert_sound_allowed(1000 + 30000, last));
	EXPECT_TRUE(!smulti::alert_sound_allowed(1000 + 30000 + 1, last));
}
