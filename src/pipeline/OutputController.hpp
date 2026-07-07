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
#include <chrono>
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
 *   - Reconnect logic runs on a dedicated m_reconnect_thread.
 *   - HealthSampler polls stats concurrently, holding a shared_ptr obtained
 *     from EndpointRegistry::controller_for().
 *   - m_state, m_last_error, m_reconnect_attempt, m_output, m_video_enc,
 *     m_audio_enc, m_reconnect_thread and m_connected_since are ALL accessed
 *     from more than one of the threads above and are guarded by m_mutex.
 *     There is no field that is mutated from two threads without the lock.
 *
 * Lazy-join rule (load-bearing — do not "simplify" this away):
 *   stop() (called on the Qt UI thread) NEVER joins m_reconnect_thread — it
 *   only sets m_stop_reconnect and force-stops the output.  Joining a thread
 *   that may be sleeping in a backoff wait, or blocked inside
 *   obs_output_start() on a socket connect, would block the UI.  The join is
 *   paid instead by whichever of these runs next:
 *     - start()             — before it re-creates encoders (Qt thread; the
 *                              wait here is bounded by the reconnect thread's
 *                              stop-flag polling, not by the network).
 *     - shutdown_blocking()  — on the ControllerReaper thread, where a long
 *                              block is fine because it is off the UI thread.
 *
 * Lifecycle of encoders vs output:
 *   start() → create encoders → attach → obs_output_start
 *   stop()  → obs_output_force_stop → release encoders (lazy-joins reconnect
 *             thread separately, see above)
 *   shutdown_blocking() → force_stop → join reconnect thread → release
 *             encoders → obs_output_release (the actual, indefinitely
 *             blocking sync point — see shutdown_blocking() doc below)
 *
 * Encoder ownership: obs_output_set_video/audio_encoder() takes its OWN
 * reference via obs_encoder_get_ref() (see libobs obs-output.c).  Our
 * obs_encoder_release() calls are therefore safe purely because of
 * refcounting — each side (OutputController and the obs_output_t) holds an
 * independent ref and releases its own, not because of any synchronization
 * with the output's internal state.
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
	 * No-op if already Live or Starting.
	 * Creates encoders, creates output, starts streaming.
	 * Returns false immediately if encoder creation fails.
	 *
	 * Joins a previous reconnect thread first if one is still around (see
	 * the "Lazy-join rule" above) — this can block the calling (Qt UI)
	 * thread briefly, but only for the bounded reconnect stop-flag poll,
	 * never indefinitely.
	 */
	bool start();

	/**
	 * Request the RTMP output to stop.
	 * Non-blocking: force-stops the output and releases encoders, but never
	 * joins m_reconnect_thread (see the "Lazy-join rule" above).  If
	 * currently reconnecting, flags the reconnect loop to abort; the thread
	 * itself is joined lazily by a later start() or by shutdown_blocking().
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

	/**
	 * SampleData — the small set of stat fields HealthSampler needs, read
	 * under a single short m_mutex lock so the sampler never dereferences
	 * m_output while shutdown_blocking() is concurrently tearing it down.
	 */
	struct SampleData {
		bool     active         {false};
		uint64_t total_bytes    {0};
		int      frames_dropped {0};
	};

	/**
	 * Encapsulated health-sample getter.  Replaces the old raw_output()
	 * accessor — obs_output_get_total_bytes/get_frames_dropped/active are
	 * cheap calls, so doing all three under one short lock window is safe
	 * and closes the use-after-free window where the sampler could otherwise
	 * read a raw obs_output_t* the reaper is concurrently releasing.
	 */
	SampleData sample() const;

	/**
	 * steady_clock timestamp of the most recent "start" signal, or the
	 * epoch (default-constructed time_point) if the output has never gone
	 * Live.  Used to compute uptime — see HealthSampler; do NOT use
	 * obs_output_get_connect_time_ms() for this, it returns the one-time
	 * RTMP handshake duration (see librtmp rtmp.c), not time-since-connect.
	 */
	std::chrono::steady_clock::time_point connected_since() const;

	/**
	 * Idempotent full teardown: force_stop, join the reconnect thread,
	 * release encoders, obs_output_release(), clear m_output.  Safe to call
	 * more than once — only the first call does any work.
	 *
	 * This is the ONLY place that calls obs_output_release() other than the
	 * destructor's safety-net call.  obs_output_release() -> obs_output_destroy()
	 * -> os_event_wait(output->stopping_event) blocks until the output's
	 * internal send thread has fully exited — indefinitely, if that thread is
	 * stuck in a socket call.  For that reason shutdown_blocking() must only
	 * ever be invoked from the ControllerReaper's worker thread, never from
	 * the Qt UI thread.  See ControllerReaper.
	 */
	void shutdown_blocking();

private:
	void set_state(OutputState s);

	/** OBS signal callbacks — registered on the output */
	static void on_start_signal(void *data, calldata_t *cd);
	static void on_stop_signal(void *data, calldata_t *cd);

	void handle_stop(int error_code);
	void reconnect_thread_func();

	/** Assumes m_mutex is already held by the caller. */
	void do_release_encoders_locked();
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

	std::chrono::steady_clock::time_point m_connected_since{};

	std::thread         m_reconnect_thread;
	std::atomic<bool>   m_stop_reconnect {false};

	/** Guards shutdown_blocking() idempotency — set once, never reset. */
	std::atomic<bool>   m_shutdown_done {false};

	static constexpr int REATTACH_WAIT_TIMEOUT_MS = 2000;
	static constexpr int REATTACH_POLL_MS         = 20;
};

} // namespace smulti
