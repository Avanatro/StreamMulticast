/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "AlertPolicy.hpp"

#include <obs-data.h>

namespace smulti {

namespace {

enum ReasonIndex { kReconnecting = 0, kFailed = 1, kDropped = 2, kLowBitrate = 3, kReasonCount = 4 };

const char *reason_name(int idx)
{
	switch (idx) {
	case kReconnecting: return kAlertReasonReconnecting;
	case kFailed:       return kAlertReasonFailed;
	case kDropped:      return kAlertReasonDropped;
	default:            return kAlertReasonLowBitrate;
	}
}

AlertLevel reason_level(int idx)
{
	return idx == kFailed ? AlertLevel::Critical : AlertLevel::Warning;
}

} // namespace

const char *alert_level_name(AlertLevel level)
{
	switch (level) {
	case AlertLevel::Warning:  return "warning";
	case AlertLevel::Critical: return "critical";
	default:                   return "none";
	}
}

const char *alert_state_name(OutputState state)
{
	switch (state) {
	case OutputState::Idle:         return "idle";
	case OutputState::Starting:     return "starting";
	case OutputState::Live:         return "live";
	case OutputState::Reconnecting: return "reconnecting";
	case OutputState::FailedHard:   return "failed";
	}
	return "idle";
}

AlertPolicy::AlertPolicy(const AlertThresholds &thresholds) : m_t(thresholds) {}

void AlertPolicy::raise(Endpoint &ep, const std::string &id, int idx, std::vector<AlertTransition> &out) const
{
	Reason &r = ep.reasons[idx];
	if (r.active)
		return;
	r.active      = true;
	r.false_since = -1;
	out.push_back({true, reason_level(idx), reason_name(idx), id, ep.name, ep.state});
}

void AlertPolicy::clear(Endpoint &ep, const std::string &id, int idx, std::vector<AlertTransition> &out) const
{
	Reason &r = ep.reasons[idx];
	if (!r.active)
		return;
	r.active      = false;
	r.false_since = -1;
	out.push_back({false, reason_level(idx), reason_name(idx), id, ep.name, ep.state});
}

/* Hysteresis for a raised alert: clears only after `condition` was false for clear_hold_ms in a row. */
void AlertPolicy::hold_clear(Endpoint &ep, const std::string &id, int idx, bool condition, int64_t now,
			     std::vector<AlertTransition> &out) const
{
	Reason &r = ep.reasons[idx];
	if (!r.active)
		return;
	if (condition) {
		r.false_since = -1;
		return;
	}
	if (r.false_since < 0)
		r.false_since = now;
	if (now - r.false_since >= m_t.clear_hold_ms)
		clear(ep, id, idx, out);
}

std::vector<AlertTransition> AlertPolicy::update(const AlertInput &in)
{
	std::vector<AlertTransition> out;
	Endpoint &ep  = m_endpoints[in.endpoint_id];
	const int64_t now = in.now_ms;
	ep.name  = in.endpoint_name;
	ep.state = in.state;

	/* Idle is a user stop: everything clears at once and the trackers restart. */
	if (in.state == OutputState::Idle) {
		for (int i = 0; i < kReasonCount; ++i)
			clear(ep, in.endpoint_id, i, out);
		ep.live_since = -1;
		ep.low_since  = -1;
		ep.window.clear();
		return out;
	}

	const bool live = in.state == OutputState::Live;
	if (live) {
		if (ep.live_since < 0)
			ep.live_since = now;
	} else {
		ep.live_since = -1;
		ep.low_since  = -1;
		ep.window.clear();
	}

	/* State alerts: immediate raise; they clear once the endpoint was Live for clear_hold_ms. */
	if (in.state == OutputState::Reconnecting)
		raise(ep, in.endpoint_id, kReconnecting, out);
	if (in.state == OutputState::FailedHard) {
		clear(ep, in.endpoint_id, kReconnecting, out);
		raise(ep, in.endpoint_id, kFailed, out);
	}
	if (live && now - ep.live_since >= m_t.clear_hold_ms) {
		clear(ep, in.endpoint_id, kReconnecting, out);
		clear(ep, in.endpoint_id, kFailed, out);
	}

	/* Drop and bitrate checks start startup_grace_ms after Live began (rate ramp, first keyframe). */
	const bool checked = live && now - ep.live_since >= m_t.startup_grace_ms;

	/* Dropped frames: ratio over the last drop_window_ms. A counter that went backwards is a new
	 * session: the window restarts from the new value, so a delta is never negative. */
	bool dropped_cond = false;
	if (checked && in.frames_total > 0) {
		if (!ep.window.empty() &&
		    (in.dropped_total < ep.window.back().dropped || in.frames_total < ep.window.back().frames))
			ep.window.clear();
		ep.window.push_back({now, in.dropped_total, in.frames_total});
		const int64_t cutoff = now - m_t.drop_window_ms;
		while (ep.window.size() >= 2 && ep.window[1].t <= cutoff)
			ep.window.erase(ep.window.begin());
		if (ep.window.size() >= 2) {
			const uint64_t dd = ep.window.back().dropped - ep.window.front().dropped;
			const uint64_t df = ep.window.back().frames - ep.window.front().frames;
			dropped_cond = df > 0 && dd >= static_cast<uint64_t>(m_t.drop_min_frames) &&
				       static_cast<double>(dd) / static_cast<double>(df) >= m_t.drop_ratio;
		}
	} else {
		ep.window.clear();
	}
	if (!ep.reasons[kDropped].active) {
		if (dropped_cond)
			raise(ep, in.endpoint_id, kDropped, out);
	} else {
		hold_clear(ep, in.endpoint_id, kDropped, dropped_cond, now, out);
	}

	/* Low bitrate: actual below a fraction of the target, continuously for low_bitrate_hold_ms. */
	const bool low = checked && in.target_kbps > 0 &&
			 in.actual_kbps < m_t.low_bitrate_fraction * static_cast<double>(in.target_kbps);
	if (low) {
		if (ep.low_since < 0)
			ep.low_since = now;
	} else {
		ep.low_since = -1;
	}
	if (!ep.reasons[kLowBitrate].active) {
		if (low && now - ep.low_since >= m_t.low_bitrate_hold_ms)
			raise(ep, in.endpoint_id, kLowBitrate, out);
	} else {
		hold_clear(ep, in.endpoint_id, kLowBitrate, low, now, out);
	}

	return out;
}

std::vector<AlertTransition> AlertPolicy::retain_only(const std::vector<std::string> &keep)
{
	std::vector<AlertTransition> out;
	for (auto it = m_endpoints.begin(); it != m_endpoints.end();) {
		bool kept = false;
		for (const auto &id : keep)
			if (id == it->first) {
				kept = true;
				break;
			}
		if (kept) {
			++it;
			continue;
		}
		for (int i = 0; i < kReasonCount; ++i)
			clear(it->second, it->first, i, out);
		it = m_endpoints.erase(it);
	}
	return out;
}

AlertStatus AlertPolicy::status(const std::string &endpoint_id) const
{
	AlertStatus st;
	auto it = m_endpoints.find(endpoint_id);
	if (it == m_endpoints.end())
		return st;
	for (int i = 0; i < kReasonCount; ++i) {
		if (!it->second.reasons[i].active)
			continue;
		st.reasons.push_back(reason_name(i));
		if (static_cast<int>(reason_level(i)) > static_cast<int>(st.level))
			st.level = reason_level(i);
	}
	return st;
}

std::map<std::string, AlertStatus> AlertPolicy::statuses() const
{
	std::map<std::string, AlertStatus> all;
	for (const auto &kv : m_endpoints) {
		AlertStatus st = status(kv.first);
		if (st.level != AlertLevel::None)
			all[kv.first] = st;
	}
	return all;
}

AlertLevel AlertPolicy::max_level() const
{
	AlertLevel max = AlertLevel::None;
	for (const auto &kv : m_endpoints) {
		const AlertLevel l = status(kv.first).level;
		if (static_cast<int>(l) > static_cast<int>(max))
			max = l;
	}
	return max;
}

obs_data_t *alert_event_payload(const AlertTransition &transition)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "endpoint_id", transition.endpoint_id.c_str());
	obs_data_set_string(data, "endpoint_name", transition.endpoint_name.c_str());
	obs_data_set_string(data, "level", alert_level_name(transition.level));
	obs_data_set_string(data, "reason", transition.reason.c_str());
	obs_data_set_bool(data, "active", transition.raised);
	obs_data_set_string(data, "state", alert_state_name(transition.state));
	return data;
}

void alert_settings_save(obs_data_t *data, const AlertSettings &settings)
{
	if (!data)
		return;
	obs_data_set_bool(data, kAlertSoundKey, settings.sound_enabled);
	obs_data_set_bool(data, kAlertWebsocketKey, settings.websocket_enabled);
}

AlertSettings alert_settings_load(obs_data_t *data)
{
	AlertSettings out;
	if (!data)
		return out;
	if (obs_data_has_user_value(data, kAlertSoundKey))
		out.sound_enabled = obs_data_get_bool(data, kAlertSoundKey);
	if (obs_data_has_user_value(data, kAlertWebsocketKey))
		out.websocket_enabled = obs_data_get_bool(data, kAlertWebsocketKey);
	return out;
}

bool alert_sound_allowed(int64_t now_ms, int64_t &last_played_ms)
{
	if (last_played_ms >= 0 && now_ms - last_played_ms < kAlertSoundMinGapMs)
		return false;
	last_played_ms = now_ms;
	return true;
}

} // namespace smulti
