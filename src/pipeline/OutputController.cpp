/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "OutputController.hpp"
#include "../plugin-support.h"

#include <obs.h>
#include <obs-output.h>
#include <obs-data.h>
#include <signal.h>
#include <util/threading.h>

#include <chrono>
#include <thread>
#include <cassert>

namespace smulti {

namespace {

/* -----------------------------------------------------------------------
 * Stop-code classification (verified against <obs-defs.h> in OBS 31.1.1):
 *   OBS_OUTPUT_SUCCESS        =  0  — intentional stop, no reconnect
 *   OBS_OUTPUT_BAD_PATH       = -1  — invalid server URL, hard fail
 *   OBS_OUTPUT_CONNECT_FAILED = -2  — reconnect-eligible
 *   OBS_OUTPUT_INVALID_STREAM = -3  — invalid/rejected stream key, hard fail
 *   OBS_OUTPUT_ERROR          = -4  — reconnect-eligible
 *   OBS_OUTPUT_DISCONNECTED   = -5  — reconnect-eligible
 *   OBS_OUTPUT_UNSUPPORTED    = -6  — hard fail
 *   OBS_OUTPUT_NO_SPACE       = -7  — hard fail
 *   OBS_OUTPUT_ENCODE_ERROR   = -8  — reconnect-eligible
 *   OBS_OUTPUT_HDR_DISABLED   = -9  — hard fail
 * ----------------------------------------------------------------------- */
bool is_hard_fail_code(int code)
{
	return code == OBS_OUTPUT_INVALID_STREAM || code == OBS_OUTPUT_BAD_PATH ||
	       code == OBS_OUTPUT_UNSUPPORTED || code == OBS_OUTPUT_NO_SPACE ||
	       code == OBS_OUTPUT_HDR_DISABLED;
}

const char *hard_fail_message(int code)
{
	switch (code) {
	case OBS_OUTPUT_INVALID_STREAM: return "Invalid stream key (rejected by server)";
	case OBS_OUTPUT_BAD_PATH:       return "Invalid server URL";
	case OBS_OUTPUT_UNSUPPORTED:    return "Unsupported output configuration";
	case OBS_OUTPUT_NO_SPACE:       return "No space left for output data";
	case OBS_OUTPUT_HDR_DISABLED:   return "HDR is disabled for this output";
	default:                        return "Output failed";
	}
}

} // anonymous namespace

/* -----------------------------------------------------------------------
 * reconnect_delay_seconds — pure function (no libobs dependency, testable)
 *
 * Backoff sequence: 1s / 2s / 5s / 10s / 30s / 60s
 * Returns -1 if attempt >= max_attempts (caller → FailedHard).
 * ----------------------------------------------------------------------- */
int reconnect_delay_seconds(int attempt, int max_attempts)
{
	if (attempt >= max_attempts)
		return -1;

	static const int schedule[] = {1, 2, 5, 10, 30, 60};
	constexpr int schedule_len  = static_cast<int>(sizeof(schedule) / sizeof(schedule[0]));

	int idx = attempt < schedule_len ? attempt : (schedule_len - 1);
	return schedule[idx];
}

/* -----------------------------------------------------------------------
 * Constructor / Destructor
 * ----------------------------------------------------------------------- */
OutputController::OutputController(const Endpoint &ep)
	: m_endpoint(ep)
{
}

OutputController::~OutputController()
{
	/* Idempotency safety net: normal teardown goes through
	 * shutdown_blocking() via the ControllerReaper (see EndpointRegistry and
	 * plugin-main.cpp).  This call is a no-op if that already happened.  It
	 * only does real (and potentially blocking) work for a controller that
	 * was destroyed without ever being handed to the reaper — e.g. a
	 * construction-failure path.  It still must not run on the Qt UI thread
	 * with a stuck output; callers are responsible for routing controllers
	 * through the reaper instead of destroying them directly on that thread. */
	shutdown_blocking();
}

/* -----------------------------------------------------------------------
 * State helpers
 * ----------------------------------------------------------------------- */
void OutputController::set_state(OutputState s)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_state = s;
}

OutputState OutputController::state() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_state;
}

bool OutputController::is_running() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_state == OutputState::Live || m_state == OutputState::Starting;
}

std::string OutputController::last_error() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_last_error;
}

OutputController::SampleData OutputController::sample() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	SampleData data;
	if (!m_output)
		return data; // active=false, zeroed — output already released or never created

	data.active = obs_output_active(m_output);
	if (data.active) {
		data.total_bytes    = obs_output_get_total_bytes(m_output);
		data.frames_dropped = obs_output_get_frames_dropped(m_output);
	}
	return data;
}

std::chrono::steady_clock::time_point OutputController::connected_since() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_connected_since;
}

/* -----------------------------------------------------------------------
 * do_create_output — creates the obs_output_t with RTMP service settings.
 * ----------------------------------------------------------------------- */
void OutputController::do_create_output()
{
	assert(!m_output);

	/* Service settings (RTMP URL + stream key) */
	obs_data_t *service_settings = obs_data_create();
	obs_data_set_string(service_settings, "server", m_endpoint.server_url.c_str());
	obs_data_set_string(service_settings, "key",    m_endpoint.stream_key.c_str());

	std::string service_name = "smulti_service_" + m_endpoint.id;
	obs_service_t *service = obs_service_create(
		"rtmp_custom",
		service_name.c_str(),
		service_settings,
		nullptr /* hotkey_data */
	);
	obs_data_release(service_settings);

	if (!service) {
		obs_log(LOG_ERROR, "OutputController [%s]: obs_service_create failed",
		        m_endpoint.name.c_str());
		return;
	}

	/* Output settings */
	obs_data_t *output_settings = obs_data_create();

	std::string output_name = "smulti_output_" + m_endpoint.id;
	obs_output_t *new_output = obs_output_create(
		"rtmp_output",
		output_name.c_str(),
		output_settings,
		nullptr /* hotkey_data */
	);
	obs_data_release(output_settings);

	if (!new_output) {
		obs_log(LOG_ERROR, "OutputController [%s]: obs_output_create failed",
		        m_endpoint.name.c_str());
		obs_service_release(service);
		return;
	}

	obs_output_set_service(new_output, service);
	obs_service_release(service); // output addref's the service

	/* Register start/stop signal handlers */
	signal_handler_t *sh = obs_output_get_signal_handler(new_output);
	signal_handler_connect(sh, "start", on_start_signal, this);
	signal_handler_connect(sh, "stop",  on_stop_signal,  this);

	std::lock_guard<std::mutex> lock(m_mutex);
	m_output = new_output;
}

/* -----------------------------------------------------------------------
 * start()
 * ----------------------------------------------------------------------- */
bool OutputController::start()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_state == OutputState::Live || m_state == OutputState::Starting)
			return true; // already running
		m_state = OutputState::Starting;
		m_last_error.clear();
		m_reconnect_attempt = 0;
	}

	obs_log(LOG_INFO, "OutputController [%s]: starting", m_endpoint.name.c_str());

	/* Lazy-join: pick up after a previous stop() or a just-finished reconnect
	 * attempt.  stop() never joins m_reconnect_thread itself (see class doc);
	 * start() pays that cost here instead, right before it needs sole
	 * ownership of m_video_enc/m_audio_enc/m_output anyway.  Flagging
	 * m_stop_reconnect first guarantees the thread (if any) exits its poll
	 * loop quickly rather than us joining an unbounded reconnect wait. */
	m_stop_reconnect.store(true);
	std::thread stale_reconnect_thread;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_reconnect_thread.joinable())
			stale_reconnect_thread = std::move(m_reconnect_thread);
	}
	if (stale_reconnect_thread.joinable())
		stale_reconnect_thread.join();
	m_stop_reconnect.store(false);

	obs_output_t *output;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		output = m_output;
	}

	/* Create output if not yet created */
	if (!output) {
		do_create_output();
		std::lock_guard<std::mutex> lock(m_mutex);
		output = m_output;
	}

	if (!output) {
		set_state(OutputState::FailedHard);
		return false;
	}

	/* Create and attach encoders */
	obs_encoder_t *video_enc = m_factory.create_video_encoder(m_endpoint, "smulti");
	obs_encoder_t *audio_enc = m_factory.create_audio_encoder(m_endpoint, "smulti");

	if (!video_enc || !audio_enc) {
		obs_log(LOG_ERROR, "OutputController [%s]: encoder creation failed",
		        m_endpoint.name.c_str());
		if (video_enc)
			obs_encoder_release(video_enc);
		if (audio_enc)
			obs_encoder_release(audio_enc);
		std::lock_guard<std::mutex> lock(m_mutex);
		m_state      = OutputState::FailedHard;
		m_last_error = "Encoder unavailable — check backend settings";
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_video_enc = video_enc;
		m_audio_enc = audio_enc;
	}

	obs_output_set_video_encoder(output, video_enc);
	obs_output_set_audio_encoder(output, audio_enc, 0 /* track index */);

	/* -------------------------------------------------------------------
	 * v1.0.5 — Per-Output Orientation (Vertical-Letterbox path)
	 *
	 * For Vertical1080x1920Letterbox we tell the output's video pipeline to
	 * convert frames to a 1080×1920 target.  OBS's internal scaler letterboxes
	 * the 16:9 main canvas inside the 9:16 frame (black bars top+bottom).
	 *
	 * Center-Crop (Vertical1080x1920CenterCrop) is reserved for v1.1 — it
	 * requires a per-output obs_view_t with custom render code that crops the
	 * centre 9:16 slice instead of letterboxing.  Disabled in the UI for now.
	 *
	 * SourceMatch: no conversion call — output uses the OBS main canvas as-is.
	 * ------------------------------------------------------------------- */
	if (m_endpoint.orientation == OutputOrientation::Vertical1080x1920Letterbox) {
		struct video_scale_info scale = {};
		scale.format     = VIDEO_FORMAT_NV12;     /* common encoder input format */
		scale.width      = 1080;
		scale.height     = 1920;
		scale.range      = VIDEO_RANGE_DEFAULT;
		scale.colorspace = VIDEO_CS_DEFAULT;
		obs_output_set_video_conversion(output, &scale);
		obs_log(LOG_INFO,
		        "OutputController [%s]: orientation=Vertical1080x1920 (Letterbox), "
		        "video conversion enabled (1080×1920)",
		        m_endpoint.name.c_str());
	}
	else if (m_endpoint.orientation == OutputOrientation::Vertical1080x1920CenterCrop) {
		/* Should not happen — UI disables this option.  Defensive fallback to letterbox. */
		obs_log(LOG_WARNING,
		        "OutputController [%s]: Vertical1080x1920CenterCrop is v1.1, "
		        "falling back to letterbox.", m_endpoint.name.c_str());
		struct video_scale_info scale = {};
		scale.format     = VIDEO_FORMAT_NV12;
		scale.width      = 1080;
		scale.height     = 1920;
		scale.range      = VIDEO_RANGE_DEFAULT;
		scale.colorspace = VIDEO_CS_DEFAULT;
		obs_output_set_video_conversion(output, &scale);
	}
	/* SourceMatch: no conversion, output uses OBS main canvas directly. */

	bool started = obs_output_start(output);
	if (!started) {
		const char *err = obs_output_get_last_error(output);
		obs_log(LOG_ERROR, "OutputController [%s]: obs_output_start failed: %s",
		        m_endpoint.name.c_str(), err ? err : "(no error)");

		std::lock_guard<std::mutex> lock(m_mutex);
		do_release_encoders_locked();
		m_state      = OutputState::FailedHard;
		m_last_error = err ? err : "RTMP connect failed";
		return false;
	}

	/* State transitions to Live via on_start_signal callback */
	return true;
}

/* -----------------------------------------------------------------------
 * stop()
 *
 * Non-blocking by design (Qt UI thread).  See the "Lazy-join rule" in the
 * class doc comment in OutputController.hpp — this method must never join
 * m_reconnect_thread.
 * ----------------------------------------------------------------------- */
void OutputController::stop()
{
	/* Flag any in-flight reconnect to abort.  The thread itself is joined
	 * lazily by a future start() or by shutdown_blocking() — never here. */
	m_stop_reconnect.store(true);

	std::lock_guard<std::mutex> lock(m_mutex);

	/* Re-check state under the lock: handle_stop() (OBS signal thread) may
	 * have changed m_state between an earlier unlocked check by the caller
	 * and this point — this closes that TOCTOU window. */
	if (m_state == OutputState::Idle)
		return;

	obs_log(LOG_INFO, "OutputController [%s]: stopping", m_endpoint.name.c_str());

	if (m_output && obs_output_active(m_output))
		obs_output_force_stop(m_output);

	do_release_encoders_locked();
	m_state = OutputState::Idle;
}

/* -----------------------------------------------------------------------
 * shutdown_blocking() — see full doc comment in OutputController.hpp.
 * ----------------------------------------------------------------------- */
void OutputController::shutdown_blocking()
{
	bool expected = false;
	if (!m_shutdown_done.compare_exchange_strong(expected, true))
		return; // already torn down (or another caller is doing it)

	m_stop_reconnect.store(true);

	obs_output_t *output_to_stop;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		output_to_stop = m_output;
	}
	if (output_to_stop && obs_output_active(output_to_stop))
		obs_output_force_stop(output_to_stop);

	/* Join the reconnect thread — this is the one place that is allowed to
	 * block on it unconditionally, because shutdown_blocking() only ever
	 * runs on the ControllerReaper's worker thread (or, as a safety net, in
	 * the destructor of a controller that was never handed to the reaper). */
	std::thread reconnect_to_join;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		reconnect_to_join = std::move(m_reconnect_thread);
	}
	if (reconnect_to_join.joinable())
		reconnect_to_join.join();

	obs_output_t *output_to_release;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		do_release_encoders_locked();
		output_to_release = m_output;
		/* Null m_output BEFORE the (potentially long) blocking release call
		 * below, while still holding the lock.  HealthSampler::sample() also
		 * locks m_mutex, so it can only ever observe m_output as either the
		 * still-valid pointer (fully before this point) or nullptr (fully
		 * after) — never a pointer to an obs_output_t mid-teardown. */
		m_output = nullptr;
		m_state  = OutputState::Idle;
	}

	if (output_to_release) {
		/* The actual, potentially indefinite synchronous point:
		 * obs_output_release() -> obs_output_destroy() ->
		 * os_event_wait(output->stopping_event), which blocks until the
		 * output's internal send thread has fully exited.  Must never run
		 * on the Qt UI thread — that is the entire reason ControllerReaper
		 * exists. */
		obs_output_release(output_to_release);
	}
}

/* -----------------------------------------------------------------------
 * do_release_encoders_locked — assumes m_mutex is already held by the caller.
 * ----------------------------------------------------------------------- */
void OutputController::do_release_encoders_locked()
{
	if (m_video_enc) {
		obs_encoder_release(m_video_enc);
		m_video_enc = nullptr;
	}
	if (m_audio_enc) {
		obs_encoder_release(m_audio_enc);
		m_audio_enc = nullptr;
	}
}

/* -----------------------------------------------------------------------
 * Signal callbacks — called on OBS's internal signal thread
 * ----------------------------------------------------------------------- */
void OutputController::on_start_signal(void *data, calldata_t * /*cd*/)
{
	auto *self = static_cast<OutputController *>(data);
	{
		std::lock_guard<std::mutex> lock(self->m_mutex);
		self->m_state           = OutputState::Live;
		self->m_connected_since = std::chrono::steady_clock::now();
	}
	obs_log(LOG_INFO, "OutputController [%s]: stream started (Live)",
	        self->m_endpoint.name.c_str());
}

void OutputController::on_stop_signal(void *data, calldata_t *cd)
{
	auto *self = static_cast<OutputController *>(data);
	int code = static_cast<int>(calldata_int(cd, "code"));
	self->handle_stop(code);
}

/* -----------------------------------------------------------------------
 * handle_stop — called from on_stop_signal (OBS signal thread)
 *
 * Decides whether to reconnect or go FailedHard based on the error code.
 * See the classification table at the top of this file (verified against
 * <obs-defs.h> for OBS 31.1.1).
 * ----------------------------------------------------------------------- */
void OutputController::handle_stop(int code)
{
	if (code == OBS_OUTPUT_SUCCESS) {
		set_state(OutputState::Idle);
		return;
	}

	if (is_hard_fail_code(code)) {
		obs_output_t *output;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			output = m_output;
		}
		const char *err = output ? obs_output_get_last_error(output) : nullptr;

		std::lock_guard<std::mutex> lock(m_mutex);
		m_last_error = (err && *err) ? err : hard_fail_message(code);
		m_state      = OutputState::FailedHard;
		obs_log(LOG_ERROR, "OutputController [%s]: hard failure (code=%d) — %s",
		        m_endpoint.name.c_str(), code, m_last_error.c_str());
		return;
	}

	/* CONNECT_FAILED / ERROR / DISCONNECTED / ENCODE_ERROR — reconnect-eligible. */
	if (m_shutdown_done.load()) {
		/* shutdown_blocking() has already claimed this controller — do not
		 * spawn new work that shutdown_blocking() would then have to race
		 * to join. */
		set_state(OutputState::Idle);
		return;
	}

	obs_log(LOG_WARNING, "OutputController [%s]: disconnected (code=%d) — reconnecting",
	        m_endpoint.name.c_str(), code);

	/* Join a stale reconnect thread (if any) before spawning a new one — this
	 * mirrors the lazy-join pattern; a stale thread here only happens if a
	 * previous reconnect attempt just returned (success or exhausted) right
	 * as a fresh disconnect signal arrived. */
	std::thread stale_thread;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_state = OutputState::Reconnecting;
		if (m_reconnect_thread.joinable())
			stale_thread = std::move(m_reconnect_thread);
	}
	if (stale_thread.joinable())
		stale_thread.join();

	m_stop_reconnect.store(false);

	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_shutdown_done.load()) {
		/* shutdown_blocking() started while we were joining the stale
		 * thread above — abort, do not spawn a thread it would have to
		 * race to join. */
		m_state = OutputState::Idle;
		return;
	}
	m_reconnect_thread = std::thread(&OutputController::reconnect_thread_func, this);
}

/* -----------------------------------------------------------------------
 * reconnect_thread_func — exponential backoff reconnect loop
 * ----------------------------------------------------------------------- */
void OutputController::reconnect_thread_func()
{
	while (!m_stop_reconnect.load()) {
		int delay = reconnect_delay_seconds(m_reconnect_attempt);
		if (delay < 0) {
			obs_log(LOG_ERROR, "OutputController [%s]: max reconnect attempts reached — FailedHard",
			        m_endpoint.name.c_str());
			std::lock_guard<std::mutex> lock(m_mutex);
			m_state      = OutputState::FailedHard;
			m_last_error = "Max reconnect attempts reached";
			return;
		}

		obs_log(LOG_INFO, "OutputController [%s]: reconnect attempt %d in %ds",
		        m_endpoint.name.c_str(), m_reconnect_attempt + 1, delay);

		/* Wait for delay, checking stop flag every 100 ms */
		for (int elapsed = 0; elapsed < delay * 10 && !m_stop_reconnect.load(); ++elapsed)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));

		if (m_stop_reconnect.load())
			break;

		++m_reconnect_attempt;

		obs_output_t *output;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			do_release_encoders_locked(); // release stale encoders before restarting
			output = m_output;
		}

		if (!output)
			continue;

		/* Re-attach guard: signal_stop() fires before the output's internal
		 * "active" flag actually clears.  Attaching encoders while the
		 * output still reports active() == true silently no-ops in libobs
		 * (see obs_output_set_video_encoder2 in obs-output.c), which would
		 * make obs_output_start() "succeed" with no encoders attached.  Wait
		 * for the output to report inactive first; if it never does within
		 * the timeout, skip this attempt rather than starting broken. */
		bool became_inactive = false;
		for (int waited_ms = 0; waited_ms < REATTACH_WAIT_TIMEOUT_MS; waited_ms += REATTACH_POLL_MS) {
			if (m_stop_reconnect.load())
				break;
			if (!obs_output_active(output)) {
				became_inactive = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(REATTACH_POLL_MS));
		}

		if (m_stop_reconnect.load())
			break;

		if (!became_inactive) {
			obs_log(LOG_WARNING,
			        "OutputController [%s]: output still active %dms after stop — "
			        "skipping reconnect attempt %d to avoid a broken re-attach",
			        m_endpoint.name.c_str(), REATTACH_WAIT_TIMEOUT_MS, m_reconnect_attempt);
			continue;
		}

		obs_encoder_t *video_enc = m_factory.create_video_encoder(m_endpoint, "smulti");
		obs_encoder_t *audio_enc = m_factory.create_audio_encoder(m_endpoint, "smulti");

		if (!video_enc || !audio_enc) {
			obs_log(LOG_ERROR, "OutputController [%s]: encoder creation failed during reconnect",
			        m_endpoint.name.c_str());
			if (video_enc)
				obs_encoder_release(video_enc);
			if (audio_enc)
				obs_encoder_release(audio_enc);
			continue;
		}

		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_video_enc = video_enc;
			m_audio_enc = audio_enc;
		}

		obs_output_set_video_encoder(output, video_enc);
		obs_output_set_audio_encoder(output, audio_enc, 0);

		bool ok = obs_output_start(output);
		if (ok) {
			obs_log(LOG_INFO, "OutputController [%s]: reconnected successfully",
			        m_endpoint.name.c_str());
			/* on_start_signal will set state to Live */
			return;
		}

		const char *err = obs_output_get_last_error(output);
		obs_log(LOG_WARNING, "OutputController [%s]: reconnect attempt %d failed: %s",
		        m_endpoint.name.c_str(), m_reconnect_attempt,
		        err ? err : "(unknown)");
	}
}

} // namespace smulti
