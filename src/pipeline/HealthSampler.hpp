/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/Endpoint.hpp"
#include "OutputController.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>
#include <memory>

namespace smulti {

// Forward declare to avoid circular include
class EndpointRegistry;

/**
 * HealthSnapshot — health data for one endpoint at one sample point.
 */
struct HealthSnapshot {
	std::string    endpoint_id;
	OutputState    state          = OutputState::Idle;
	int            target_bitrate = 0;      ///< kbps from Endpoint config
	double         actual_bitrate = 0.0;    ///< kbps, rolling 5s avg
	uint64_t       dropped_frames = 0;      ///< cumulative total
	int            reconnect_count = 0;     ///< incremented per reconnect attempt
	int64_t        uptime_sec     = 0;      ///< seconds since output started
	std::string    last_error;
};

/**
 * HealthSampler — background 2-Hz polling thread.
 *
 * Polls all active OutputController instances every 500 ms.
 * Computes rolling 5-second bitrate average from obs_output_get_total_bytes diffs.
 * Results are stored in a mutex-protected map; UI reads via snapshot().
 *
 * Threading:
 *   - start() launches a std::thread.
 *   - stop() sets running_ = false and joins (blocking, max ~600 ms).
 *   - snapshot() is safe to call from any thread (Qt UI thread included).
 *   - UI updates must still be dispatched via QMetaObject::invokeMethod
 *     (Qt::QueuedConnection) by the HealthTab — HealthSampler only stores
 *     data; it does not touch Qt objects directly.
 *
 * poll_loop() holds a shared_ptr<OutputController> (from
 * EndpointRegistry::controller_for()) for the duration of each sample, and
 * reads all obs_output_t* stats through OutputController::sample() — a
 * single short lock window inside OutputController, never a raw obs_output_t*
 * held here.  This closes a use-after-free: EndpointRegistry::remove()/
 * update() can hand a controller to the ControllerReaper for teardown at any
 * time, and shutdown_blocking() nulls the controller's m_output under its
 * own mutex before releasing it — HealthSampler can therefore only ever see
 * the output as fully present or fully gone, never mid-teardown.
 */
class HealthSampler {
public:
	explicit HealthSampler(EndpointRegistry &registry);
	~HealthSampler();

	/* Non-copyable */
	HealthSampler(const HealthSampler &) = delete;
	HealthSampler &operator=(const HealthSampler &) = delete;

	/** Start the 2-Hz polling thread */
	void start();

	/** Stop the polling thread (blocks until joined) */
	void stop();

	/**
	 * Snapshot of health data for all known endpoints.
	 * Returns a copy of the current map — safe for UI thread consumption.
	 */
	std::unordered_map<std::string, HealthSnapshot> snapshot() const;

	/**
	 * Snapshot for a single endpoint (by UUID).
	 * Returns a default-constructed HealthSnapshot if not found.
	 */
	HealthSnapshot snapshot_for(const std::string &endpoint_id) const;

private:
	void poll_loop();
	void sample_output(const Endpoint &ep, const std::shared_ptr<OutputController> &ctrl);

	EndpointRegistry &m_registry;

	mutable std::mutex m_mutex;
	std::unordered_map<std::string, HealthSnapshot> m_snapshots;

	/* Per-output rolling bitrate state */
	struct BitrateState {
		uint64_t last_bytes     = 0;
		std::chrono::steady_clock::time_point last_time;
		std::vector<double> samples; ///< rolling 5s window (10 samples at 2 Hz)
	};
	std::unordered_map<std::string, BitrateState> m_bitrate_state;

	std::atomic<bool> m_running {false};
	std::thread       m_thread;

	static constexpr int POLL_INTERVAL_MS = 500;   ///< 2 Hz
	static constexpr int ROLLING_SAMPLES  = 10;    ///< 5s @ 2 Hz
};

} // namespace smulti
