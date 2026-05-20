/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/Endpoint.hpp"
#include "EncoderFactory.hpp"

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <vector>
#include <cstdint>

// libobs forward declarations
struct obs_output;
typedef struct obs_output obs_output_t;
struct obs_encoder;
typedef struct obs_encoder obs_encoder_t;
struct calldata;
typedef struct calldata calldata_t;

namespace smulti {

/**
 * OutputState — state machine for a single RTMP output.
 *
 * Transitions:
 *   Idle → Starting → Live → Reconnecting → Live
 *                          ↘ FailedHard
 *   Any → Idle (via stop())
 */
enum class OutputState : int {
	Idle         = 0,
	Starting     = 1,
	Live         = 2,
	Reconnecting = 3,
	FailedHard   = 4,
};

/**
 * ReconnectSchedule — pure function, testable without libobs.
 *
 * Given an attempt number (0-based), returns the delay in seconds.
 * Backoff sequence: 1 / 2 / 5 / 10 / 30 / 60 (capped at 60).
 * After max_attempts, returns -1 (caller should go FailedHard).
 */
int reconnect_delay_seconds(int attempt, int max_attempts = 10);

/**
 * OutputController — manages one obs_output_t (rtmp_output) for one Endpoint.
 *
 * Threading model:
 *   - start() / stop() are called from the Qt UI thread (via EndpointRegistry).
 *   - Reconnect logic runs on a dedicated reconnect_thread_.
 *   - state_ and last_error_ are accessed from both threads — guarded by mutex_.
 *   - HealthSampler reads stats from obs_output_t* directly (obs API is
 *     thread-safe for read-only stat calls — AVANATRO-VERIFY below).
 *
 * Lifecycle of encoders vs output:
 *   start() → create encoders → attach → obs_output_start
 *   stop()  → obs_output_stop → wait for signal → obs_encoder_release
 *
 * AVANATRO-VERIFY: Encoder release ordering.
 * The safe pattern is: stop the output, then release encoders.
 * obs_output holds refs to its encoders — releasing before stop may cause
 * use-after-free.  Confirm with OBS source (obs-output.c).
 */
class OutputController {
public:
	explicit OutputController(const Endpoint &ep);
	~OutputController();

	/* Non-copyable, non-movable */
	OutputController(const OutputController &) = delete;
	OutputController &operator=(const OutputController &) = delete;

	/**
	 * Start the RTMP output for this endpoint.
	 * No-op if already running.
	 * Creates encoders, creates output, starts streaming.
	 * Returns false immediately if encoder creation fails.
	 *
	 * AVANATRO-VERIFY: obs_output_create — the output type "rtmp_output"
	 * supports both rtmp:// and rtmps://.  Confirm in OBS 31.x (was true in 30).
	 */
	bool start();

	/**
	 * Stop the RTMP output.
	 * Synchronous: waits for obs_output_stop signal before releasing encoders.
	 * If currently reconnecting, cancels the reconnect.
	 *
	 * AVANATRO-VERIFY: obs_output_stop — confirm this is non-blocking (fires
	 * a signal asynchronously) and that we should use obs_output_get_state()
	 * or a signal to know when it's truly stopped before releasing encoders.
	 * Alternative: obs_output_force_stop() for immediate termination.
	 */
	void stop();

	/** Is the output currently live (OutputState::Live)? */
	bool is_running() const;

	/** Current state */
	OutputState state() const;

	/** Last error string (empty if no error) */
	std::string last_error() const;

	/** Endpoint ID this controller manages */
	const std::string &endpoint_id() const { return m_endpoint.id; }

	/** The obs_output_t* — used by HealthSampler for stat polling.
	 *  May be nullptr if not started yet. Caller must not release this ptr.
	 *
	 *  AVANATRO-VERIFY: Thread-safe read of obs_output_get_total_bytes etc.
	 *  OBS stat read functions are documented as thread-safe; confirm for 31.x.
	 */
	obs_output_t *raw_output() const;

private:
	void set_state(OutputState s);

	/** OBS signal callbacks — registered on the output */
	static void on_start_signal(void *data, calldata_t *cd);
	static void on_stop_signal(void *data, calldata_t *cd);

	void handle_stop(int error_code);
	void reconnect_thread_func();
	void do_release_encoders();
	void do_create_output();

	Endpoint            m_endpoint;
	EncoderFactory      m_factory;

	mutable std::mutex  m_mutex;
	OutputState         m_state    {OutputState::Idle};
	std::string         m_last_error;
	int                 m_reconnect_attempt {0};

	obs_output_t       *m_output   {nullptr};
	obs_encoder_t      *m_video_enc{nullptr};
	obs_encoder_t      *m_audio_enc{nullptr};

	std::thread         m_reconnect_thread;
	std::atomic<bool>   m_stop_reconnect {false};
};

} // namespace smulti
