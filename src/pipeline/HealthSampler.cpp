/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "HealthSampler.hpp"
#include "../core/EndpointRegistry.hpp"
#include "../plugin-support.h"

#include <numeric>
#include <chrono>
#include <thread>

namespace smulti {

/* -----------------------------------------------------------------------
 * Constructor / Destructor
 * ----------------------------------------------------------------------- */
HealthSampler::HealthSampler(EndpointRegistry &registry)
	: m_registry(registry)
{
}

HealthSampler::~HealthSampler()
{
	stop();
}

/* -----------------------------------------------------------------------
 * start() / stop()
 * ----------------------------------------------------------------------- */
void HealthSampler::start()
{
	if (m_running.load())
		return;
	m_running.store(true);
	m_thread = std::thread(&HealthSampler::poll_loop, this);
	obs_log(LOG_INFO, "HealthSampler: started (2 Hz)");
}

void HealthSampler::stop()
{
	if (!m_running.load())
		return;
	m_running.store(false);
	if (m_thread.joinable())
		m_thread.join();
	obs_log(LOG_INFO, "HealthSampler: stopped");
}

/* -----------------------------------------------------------------------
 * snapshot()
 * ----------------------------------------------------------------------- */
std::unordered_map<std::string, HealthSnapshot> HealthSampler::snapshot() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_snapshots;
}

HealthSnapshot HealthSampler::snapshot_for(const std::string &endpoint_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	auto it = m_snapshots.find(endpoint_id);
	if (it != m_snapshots.end())
		return it->second;
	return HealthSnapshot{};
}

/* -----------------------------------------------------------------------
 * poll_loop — 2-Hz background thread
 * ----------------------------------------------------------------------- */
void HealthSampler::poll_loop()
{
	while (m_running.load()) {
		auto start = std::chrono::steady_clock::now();

		/* Get all endpoints from registry (returns a copy — safe) */
		auto endpoints = m_registry.all();

		for (const auto &ep : endpoints) {
			/* M3: disabled endpoints are guaranteed non-running —
			 * ConfigTab::on_toggle_endpoint() stops the output the moment
			 * `enabled` flips to false, and a disabled endpoint is never
			 * started in the first place.  Skip the controller_for() lookup
			 * and OutputController's locked state()/last_error() getters
			 * entirely and write a cheap Idle snapshot directly. */
			if (!ep.enabled) {
				write_inactive_snapshot(ep, "", OutputState::Idle);
				continue;
			}

			/* shared_ptr copy — keeps the controller alive for this sample
			 * even if EndpointRegistry concurrently removes/replaces it and
			 * hands it to the ControllerReaper. */
			auto ctrl = m_registry.controller_for(ep.id);
			sample_output(ep, ctrl);
		}

		/* Sleep remainder of interval */
		auto elapsed = std::chrono::steady_clock::now() - start;
		auto sleep_time = std::chrono::milliseconds(POLL_INTERVAL_MS) - elapsed;
		if (sleep_time.count() > 0)
			std::this_thread::sleep_for(sleep_time);
	}
}

/* -----------------------------------------------------------------------
 * sample_output — collect health data for one endpoint
 *
 * All obs_output_t access goes through OutputController::sample() /
 * connected_since() — see the class doc comment in HealthSampler.hpp for
 * why this indirection exists (it is not just style: it closes a real
 * use-after-free against the ControllerReaper).
 * ----------------------------------------------------------------------- */
void HealthSampler::sample_output(const Endpoint &ep, const std::shared_ptr<OutputController> &ctrl)
{
	std::string last_error = ctrl ? ctrl->last_error() : "";
	OutputState state      = ctrl ? ctrl->state() : OutputState::Idle;

	if (!ctrl || state == OutputState::Idle || state == OutputState::FailedHard) {
		write_inactive_snapshot(ep, last_error, state);
		return;
	}

	OutputController::SampleData sample = ctrl->sample();
	if (!sample.active) {
		/* M1 fix: this branch is reached for Reconnecting (output detached/
		 * not yet re-attached) and for Starting before the output has
		 * actually gone active — both are "not currently streaming" just
		 * like Idle/FailedHard above, so the bitrate rolling window must be
		 * reset here too. Previously only the Idle/FailedHard branch erased
		 * m_bitrate_state, so a Reconnecting endpoint kept its stale
		 * last_bytes. On reconnect, obs_output_get_total_bytes() restarts
		 * near zero, and byte_diff = total - last_bytes (both uint64_t)
		 * underflowed into a huge bogus value that then polluted the
		 * rolling ~10-sample kbps average for several seconds. */
		write_inactive_snapshot(ep, last_error, state);
		return;
	}

	HealthSnapshot snap;
	snap.endpoint_id    = ep.id;
	snap.target_bitrate = ep.video_bitrate_kbps + ep.audio_bitrate_kbps;
	snap.last_error     = last_error;
	snap.state          = state;

	/* Bytes sent — compute diff for bitrate rolling average */
	uint64_t total_bytes = sample.total_bytes;
	uint64_t dropped     = static_cast<uint64_t>(sample.frames_dropped);

	auto now = std::chrono::steady_clock::now();

	BitrateState &bstate = m_bitrate_state[ep.id];
	if (bstate.last_bytes > 0) {
		uint64_t byte_diff = total_bytes - bstate.last_bytes;
		double elapsed_sec = std::chrono::duration<double>(now - bstate.last_time).count();
		if (elapsed_sec > 0.0) {
			double kbps = (static_cast<double>(byte_diff) * 8.0) / elapsed_sec / 1000.0;
			bstate.samples.push_back(kbps);
			if (static_cast<int>(bstate.samples.size()) > ROLLING_SAMPLES)
				bstate.samples.erase(bstate.samples.begin());
		}
	}
	bstate.last_bytes = total_bytes;
	bstate.last_time  = now;

	/* Rolling average bitrate */
	if (!bstate.samples.empty()) {
		double sum = std::accumulate(bstate.samples.begin(), bstate.samples.end(), 0.0);
		snap.actual_bitrate = sum / static_cast<double>(bstate.samples.size());
	}

	snap.dropped_frames = dropped;

	/* Uptime — computed from OutputController's own "start" signal timestamp
	 * (std::chrono::steady_clock), NOT obs_output_get_connect_time_ms(): that
	 * function returns the one-time RTMP handshake duration (set once, see
	 * librtmp rtmp.c ~line 1112), not time-since-connect, which is why the
	 * uptime column previously always showed ~0. */
	auto connected_since = ctrl->connected_since();
	if (connected_since.time_since_epoch().count() != 0) {
		auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - connected_since).count();
		snap.uptime_sec = uptime > 0 ? uptime : 0;
	}

	/* Reconnect count — we count it from the reconnect_attempt field via state */
	/* We don't have direct access to m_reconnect_attempt here (it's private).
	 * In v1.1 expose it via a public getter on OutputController. */
	snap.reconnect_count = 0; /* TODO(v1.1): expose reconnect count from OutputController */

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_snapshots[ep.id] = snap;
	}
}

/* -----------------------------------------------------------------------
 * write_inactive_snapshot — cheap zeroed snapshot + bitrate-window reset
 * for any endpoint that is not currently streaming.  See the class doc
 * comment in HealthSampler.hpp: never dereferences obs_output_t itself —
 * `last_error`/`state` are either OutputController::last_error()/state()'s
 * already-locked return values, or defaults for an endpoint with no
 * controller lookup at all (M3's disabled-endpoint fast path).
 * ----------------------------------------------------------------------- */
void HealthSampler::write_inactive_snapshot(const Endpoint &ep, const std::string &last_error,
                                             OutputState state)
{
	HealthSnapshot snap;
	snap.endpoint_id     = ep.id;
	snap.target_bitrate  = ep.video_bitrate_kbps + ep.audio_bitrate_kbps;
	snap.last_error      = last_error;
	snap.state           = state;
	snap.actual_bitrate  = 0.0;
	snap.dropped_frames  = 0;
	snap.reconnect_count = 0;
	snap.uptime_sec      = 0;

	std::lock_guard<std::mutex> lock(m_mutex);
	m_snapshots[ep.id] = snap;
	/* Clear bitrate rolling window on every transition to inactive — see the
	 * M1 fix note above sample_output()'s `!sample.active` branch. */
	m_bitrate_state.erase(ep.id);
}

} // namespace smulti
