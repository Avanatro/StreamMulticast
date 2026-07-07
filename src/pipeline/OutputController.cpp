/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "OutputController.hpp"
#include "ControllerReaper.hpp"
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
OutputController::OutputController(const Endpoint &ep, ControllerReaper &reaper)
	: m_endpoint(ep)
	, m_reaper(reaper)
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
 * Non-blocking by design (Qt UI thread).  See the class doc comment's
 * "Detach-and-reaper rule" in OutputController.hpp (fix-round 1,
 * 2026-07-07) — this method must never join m_reconnect_thread and must
 * never call a blocking libobs function itself; both are handed to the
 * ControllerReaper as one job.
 * ----------------------------------------------------------------------- */
void OutputController::stop()
{
	/* Flag any in-flight reconnect to abort — checked by
	 * reconnect_thread_func()'s poll loop and its session-validity guards. */
	m_stop_reconnect.store(true);

	obs_output_t *output_for_signals;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_state == OutputState::Idle && !m_output)
			return; // nothing to do
		output_for_signals = m_output;
	}

	/* Disconnect the output's "start"/"stop" signal handlers BEFORE
	 * re-taking m_mutex below, and deliberately OUTSIDE any lock on it.
	 *
	 * Why outside the lock: signal_handler_disconnect() locks libobs's own
	 * per-signal mutex (see obs-studio-31.1.1/libobs/callback/signal.c,
	 * signal_handler_disconnect()/signal_handler_signal()) — and that same
	 * mutex is held by signal_handler_signal() for the *entire* callback
	 * loop of an emission, i.e. for the whole duration of on_start_signal()/
	 * on_stop_signal(), which themselves lock our m_mutex.  Calling
	 * disconnect() while WE hold m_mutex would be a textbook AB-BA lock-order
	 * inversion against the OBS signal thread (us: m_mutex -> libobs signal
	 * mutex; signal thread: libobs signal mutex -> m_mutex) — a real
	 * deadlock, verified against the vendored libobs source, not a
	 * hypothetical one.
	 *
	 * Once signal_handler_disconnect() returns here, no on_start_signal/
	 * on_stop_signal invocation for this output is either in flight or will
	 * ever fire again (per the same source: a disconnect during an active
	 * emission marks the callback for removal, and that removal completes,
	 * still under the signal's own lock, before the emitting thread releases
	 * it — so disconnect() blocks until any such removal is done). */
	if (output_for_signals) {
		signal_handler_t *sh = obs_output_get_signal_handler(output_for_signals);
		signal_handler_disconnect(sh, "start", on_start_signal, this);
		signal_handler_disconnect(sh, "stop",  on_stop_signal,  this);
	}

	obs_output_t  *output_to_release;
	obs_encoder_t *video_to_release;
	obs_encoder_t *audio_to_release;
	std::thread    reconnect_to_join;

	{
		std::lock_guard<std::mutex> lock(m_mutex);

		/* Re-check under the lock: another stop() call, or handle_stop()'s
		 * hard-fail path, may have already detached/idled the session while
		 * we were disconnecting signals above. */
		if (m_state == OutputState::Idle && !m_output)
			return;

		obs_log(LOG_INFO, "OutputController [%s]: stopping", m_endpoint.name.c_str());

		/* Detach: extract everything into locals and clear our own fields.
		 * From the caller's point of view the controller is Idle again
		 * immediately — the actual (potentially blocking) teardown happens
		 * on the reaper thread below. */
		output_to_release = m_output;
		video_to_release  = m_video_enc;
		audio_to_release  = m_audio_enc;
		reconnect_to_join = std::move(m_reconnect_thread);

		m_output          = nullptr;
		m_video_enc       = nullptr;
		m_audio_enc       = nullptr;
		m_state           = OutputState::Idle;
		m_connected_since = std::chrono::steady_clock::time_point{};
	}

	/* Hand the detached session to the ControllerReaper as one job.  `self`
	 * pins this controller alive until the job runs: reconnect_thread_func()
	 * — if it is still unwinding after observing m_stop_reconnect / the
	 * session-validity guard — keeps touching this-members
	 * (m_reconnect_attempt, m_factory, ...) until it returns, and the job
	 * below joins it before touching anything else.
	 *
	 * std::thread is move-only, but std::function<void()> requires a
	 * copy-constructible target — wrap it in a shared_ptr so the lambda
	 * stays copyable (the reaper's queue only needs to move std::function
	 * itself, but std::function's own type-erased storage still requires
	 * this). */
	auto self               = shared_from_this();
	auto reconnect_to_join_p = std::make_shared<std::thread>(std::move(reconnect_to_join));
	m_reaper.enqueue([self, output_to_release, video_to_release, audio_to_release,
	                  reconnect = std::move(reconnect_to_join_p)]() {
		if (reconnect->joinable())
			reconnect->join();

		/* Unconditional — no obs_output_active() gate.  obs_output_force_stop()
		 * itself only checks its own "stopping" dedup flag (see
		 * obs-output.c), not active(); the rtmp_output module's stop path
		 * joins its own connect thread even mid-connection-attempt, which is
		 * exactly what closes the "stop() during the very first connect
		 * attempt used to no-op" bug (obs_output_active() is false during
		 * that window, so the old gate skipped force_stop entirely). */
		if (output_to_release)
			obs_output_force_stop(output_to_release);

		if (video_to_release)
			obs_encoder_release(video_to_release);
		if (audio_to_release)
			obs_encoder_release(audio_to_release);

		if (output_to_release)
			obs_output_release(output_to_release);
	});
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

	/* Detach the session up front, mirroring stop()'s ordering (fix-round 2,
	 * 2026-07-07): disconnect the output's signal handlers, null m_output (and
	 * the other owned session fields) under m_mutex, join the reconnect thread,
	 * and only THEN force_stop + release the local copies.
	 *
	 * Why this ordering, even though this path only runs on the reaper/unload
	 * thread: a direct caller that skips stop() (e.g. the destructor safety net,
	 * or a future caller) must not defeat reconnect_thread_func()'s identity
	 * guard.  That guard re-checks `m_output == captured_output` under m_mutex
	 * before obs_output_start().  If we force_stop()ed before nulling m_output
	 * and joining, the reconnect thread could still observe m_output ==
	 * captured and race obs_output_start() against our obs_output_force_stop()
	 * on the same obs_output_t (UB).  By nulling m_output before force_stop, the
	 * guard is guaranteed to observe the detach and bail out. */
	obs_output_t *output_for_signals;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		output_for_signals = m_output;
	}

	/* Disconnect the "start"/"stop" signal handlers OUTSIDE m_mutex — same
	 * AB-BA lock-order rationale documented in stop() above (libobs holds its
	 * per-signal mutex across the whole callback loop, and our callbacks lock
	 * m_mutex; disconnecting under m_mutex would invert that order). */
	if (output_for_signals) {
		signal_handler_t *sh = obs_output_get_signal_handler(output_for_signals);
		signal_handler_disconnect(sh, "start", on_start_signal, this);
		signal_handler_disconnect(sh, "stop",  on_stop_signal,  this);
	}

	/* Extract the session into locals and null our own fields under m_mutex.
	 * From here on reconnect_thread_func()'s identity guard sees m_output ==
	 * nullptr and bails before touching the captured output. */
	obs_output_t  *output_to_release;
	obs_encoder_t *video_to_release;
	obs_encoder_t *audio_to_release;
	std::thread    reconnect_to_join;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		output_to_release = m_output;
		video_to_release  = m_video_enc;
		audio_to_release  = m_audio_enc;
		reconnect_to_join = std::move(m_reconnect_thread);

		/* Null m_output BEFORE the (potentially long) blocking release call
		 * below, while still holding the lock.  HealthSampler::sample() also
		 * locks m_mutex, so it can only ever observe m_output as either the
		 * still-valid pointer (fully before this point) or nullptr (fully
		 * after) — never a pointer to an obs_output_t mid-teardown. */
		m_output    = nullptr;
		m_video_enc = nullptr;
		m_audio_enc = nullptr;
		m_state     = OutputState::Idle;
	}

	/* Join the reconnect thread — this is the one place that is allowed to
	 * block on it unconditionally, because shutdown_blocking() only ever
	 * runs on the ControllerReaper's worker thread (or, as a safety net, in
	 * the destructor of a controller that was never handed to the reaper).
	 * The thread has already observed the nulled m_output (or m_stop_reconnect)
	 * and is unwinding without touching the output further. */
	if (reconnect_to_join.joinable())
		reconnect_to_join.join();

	/* Unconditional — no obs_output_active() gate.  Same "connecting window"
	 * argument as stop() (see its doc comment above): obs_output_force_stop()
	 * itself doesn't require active() to be true, and this path only ever
	 * runs on the reaper/unload thread anyway, so there's no UI-blocking
	 * concern to trade off against by calling it unconditionally. */
	if (output_to_release)
		obs_output_force_stop(output_to_release);

	if (video_to_release)
		obs_encoder_release(video_to_release);
	if (audio_to_release)
		obs_encoder_release(audio_to_release);

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
 * reconnect_thread_func — exponential backoff reconnect loop.
 * See the session-validity guard doc comment on the declaration in
 * OutputController.hpp for the full rationale (fix-round 1, 2026-07-07).
 * ----------------------------------------------------------------------- */
void OutputController::reconnect_thread_func()
{
	obs_output_t *captured_output;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		captured_output = m_output;
	}
	if (!captured_output)
		return; // session already detached before this thread got to run

	/* Session-change probe used by the wait loops below (fix-round 2,
	 * 2026-07-07): returns true once stop()/shutdown_blocking() has nulled or
	 * replaced m_output.  The loops previously polled only m_stop_reconnect;
	 * if a real disconnect's handle_stop() had reset that flag back to false,
	 * a concurrent detach would go unnoticed until the identity check after the
	 * next FULL backoff (~1s), stalling a ControllerReaper job's join for that
	 * long.  Checking m_output != captured_output each 100ms poll iteration
	 * (under a short lock) closes that stall. */
	auto session_changed = [this, captured_output]() {
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_output != captured_output;
	};

	while (!m_stop_reconnect.load()) {
		int delay = reconnect_delay_seconds(m_reconnect_attempt);
		if (delay < 0) {
			obs_log(LOG_ERROR, "OutputController [%s]: max reconnect attempts reached — FailedHard",
			        m_endpoint.name.c_str());
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_output == captured_output) {
				m_state      = OutputState::FailedHard;
				m_last_error = "Max reconnect attempts reached";
			}
			return;
		}

		obs_log(LOG_INFO, "OutputController [%s]: reconnect attempt %d in %ds",
		        m_endpoint.name.c_str(), m_reconnect_attempt + 1, delay);

		/* Wait for delay, checking stop flag AND session validity every 100 ms */
		for (int elapsed = 0;
		     elapsed < delay * 10 && !m_stop_reconnect.load() && !session_changed();
		     ++elapsed)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));

		if (m_stop_reconnect.load() || session_changed())
			break;

		++m_reconnect_attempt;

		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_stop_reconnect.load() || m_output != captured_output)
				return; // stop() (or a fresh start()) already detached this session
			do_release_encoders_locked(); // release stale encoders before restarting
		}

		/* Re-attach guard: signal_stop() fires before the output's internal
		 * "active" flag actually clears.  Attaching encoders while the
		 * output still reports active() == true silently no-ops in libobs
		 * (see obs_output_set_video_encoder2 in obs-output.c), which would
		 * make obs_output_start() "succeed" with no encoders attached.  Wait
		 * for the output to report inactive first; if it never does within
		 * the timeout, skip this attempt rather than starting broken. */
		bool became_inactive = false;
		for (int waited_ms = 0; waited_ms < REATTACH_WAIT_TIMEOUT_MS; waited_ms += REATTACH_POLL_MS) {
			if (m_stop_reconnect.load() || session_changed())
				break;
			if (!obs_output_active(captured_output)) {
				became_inactive = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(REATTACH_POLL_MS));
		}

		if (m_stop_reconnect.load() || session_changed())
			break;

		if (!became_inactive) {
			obs_log(LOG_WARNING,
			        "OutputController [%s]: output still active %dms after stop — "
			        "skipping reconnect attempt %d to avoid a broken re-attach",
			        m_endpoint.name.c_str(), REATTACH_WAIT_TIMEOUT_MS, m_reconnect_attempt);
			continue;
		}

		/* Session-validity guard #1 — before creating encoders.  If stop()
		 * detached this session (or start() reclaimed it) while we were
		 * waiting above, bail out now rather than creating encoders for a
		 * session nobody owns any more. */
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_stop_reconnect.load() || m_output != captured_output)
				return;
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

		/* Session-validity guard #2 — immediately before obs_output_start().
		 * This is the exact race Finding A.2 described: without this check,
		 * a stop() that ran during encoder creation above could otherwise be
		 * raced by obs_output_start() below on an output stop() has already
		 * handed to the ControllerReaper.  If invalid, release the
		 * just-created, not-yet-attached encoders and exit without touching
		 * shared state further — the reaper owns the captured output now. */
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_stop_reconnect.load() || m_output != captured_output) {
				obs_encoder_release(video_enc);
				obs_encoder_release(audio_enc);
				return;
			}
			m_video_enc = video_enc;
			m_audio_enc = audio_enc;
		}

		obs_output_set_video_encoder(captured_output, video_enc);
		obs_output_set_audio_encoder(captured_output, audio_enc, 0);

		bool ok = obs_output_start(captured_output);
		if (ok) {
			obs_log(LOG_INFO, "OutputController [%s]: reconnected successfully",
			        m_endpoint.name.c_str());
			/* on_start_signal will set state to Live */
			return;
		}

		/* Re-check once more after obs_output_start() returns: if stop() ran
		 * while obs_output_start() was in flight, do nothing further — the
		 * output's signal handlers are already disconnected, and the reaper
		 * owns cleanup of the now-detached captured_output. */
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_output != captured_output)
				return;
		}

		const char *err = obs_output_get_last_error(captured_output);
		obs_log(LOG_WARNING, "OutputController [%s]: reconnect attempt %d failed: %s",
		        m_endpoint.name.c_str(), m_reconnect_attempt,
		        err ? err : "(unknown)");
	}
}

} // namespace smulti
