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
	/* Ensure graceful shutdown */
	m_stop_reconnect.store(true);
	if (m_reconnect_thread.joinable())
		m_reconnect_thread.join();

	if (m_output && obs_output_active(m_output)) {
		// AVANATRO-VERIFY: obs_output_force_stop vs obs_output_stop in destructor.
		// force_stop is synchronous; stop is async.  In destructor context,
		// force_stop is safer to avoid UAF on the signal handlers.
		obs_output_force_stop(m_output);
	}

	do_release_encoders();

	if (m_output) {
		obs_output_release(m_output);
		m_output = nullptr;
	}
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

obs_output_t *OutputController::raw_output() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_output;
}

/* -----------------------------------------------------------------------
 * do_create_output — creates the obs_output_t with RTMP service settings.
 *
 * AVANATRO-VERIFY: obs_output_create — type "rtmp_output" supports both
 * rtmp:// and rtmps://.  Confirm still valid in OBS 31.x.
 *
 * AVANATRO-VERIFY: obs_service_t vs direct obs_output_update for RTMP URL/key.
 * In OBS 30+ the recommended approach for custom outputs is to set the
 * RTMP URL/key via obs_output_set_service with a custom obs_service_t
 * (type "rtmp_custom"), OR to use obs_output_update with "server" and "key"
 * data fields.  Using obs_service_t is more correct — verify which approach
 * obs_output_t "rtmp_output" expects in OBS 31.x.
 * ----------------------------------------------------------------------- */
void OutputController::do_create_output()
{
	assert(!m_output);

	/* Service settings (RTMP URL + stream key) */
	obs_data_t *service_settings = obs_data_create();
	obs_data_set_string(service_settings, "server", m_endpoint.server_url.c_str());
	obs_data_set_string(service_settings, "key",    m_endpoint.stream_key.c_str());

	// AVANATRO-VERIFY: obs_service_create — "rtmp_custom" is the correct type for
	// user-supplied RTMP URLs.  Confirm name doesn't conflict with OBS's main service.
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
	m_output = obs_output_create(
		"rtmp_output",
		output_name.c_str(),
		output_settings,
		nullptr /* hotkey_data */
	);
	obs_data_release(output_settings);

	if (!m_output) {
		obs_log(LOG_ERROR, "OutputController [%s]: obs_output_create failed",
		        m_endpoint.name.c_str());
		obs_service_release(service);
		return;
	}

	obs_output_set_service(m_output, service);
	obs_service_release(service); // output addref's the service

	/* Register start/stop signal handlers */
	signal_handler_t *sh = obs_output_get_signal_handler(m_output);
	// AVANATRO-VERIFY: signal_handler_connect — confirm still valid in OBS 31.x.
	// This is a libobs internal signal API (util/signal.h).
	signal_handler_connect(sh, "start", on_start_signal, this);
	signal_handler_connect(sh, "stop",  on_stop_signal,  this);
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

	/* Create output if not yet created */
	if (!m_output)
		do_create_output();

	if (!m_output) {
		set_state(OutputState::FailedHard);
		return false;
	}

	/* Create and attach encoders */
	m_video_enc = m_factory.create_video_encoder(m_endpoint, "smulti");
	m_audio_enc = m_factory.create_audio_encoder(m_endpoint, "smulti");

	if (!m_video_enc || !m_audio_enc) {
		obs_log(LOG_ERROR, "OutputController [%s]: encoder creation failed",
		        m_endpoint.name.c_str());
		do_release_encoders();
		set_state(OutputState::FailedHard);
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_last_error = "Encoder unavailable — check backend settings";
		}
		return false;
	}

	// AVANATRO-VERIFY: obs_output_set_video_encoder / obs_output_set_audio_encoder.
	// In OBS 30+ these functions attach encoders to the output.
	// They must be called before obs_output_start().
	// Confirm they don't addref (ownership stays with OutputController) or do addref
	// (and we must release our handle after attaching).
	// Current assumption: output does NOT take ownership — we must release on stop.
	obs_output_set_video_encoder(m_output, m_video_enc);
	obs_output_set_audio_encoder(m_output, m_audio_enc, 0 /* track index */);

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
		obs_output_set_video_conversion(m_output, &scale);
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
		obs_output_set_video_conversion(m_output, &scale);
	}
	/* SourceMatch: no conversion, output uses OBS main canvas directly. */

	bool started = obs_output_start(m_output);
	if (!started) {
		const char *err = obs_output_get_last_error(m_output);
		obs_log(LOG_ERROR, "OutputController [%s]: obs_output_start failed: %s",
		        m_endpoint.name.c_str(), err ? err : "(no error)");
		do_release_encoders();
		set_state(OutputState::FailedHard);
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_last_error = err ? err : "RTMP connect failed";
		}
		return false;
	}

	/* State transitions to Live via on_start_signal callback */
	return true;
}

/* -----------------------------------------------------------------------
 * stop()
 * ----------------------------------------------------------------------- */
void OutputController::stop()
{
	/* Cancel any in-progress reconnect */
	m_stop_reconnect.store(true);
	if (m_reconnect_thread.joinable()) {
		m_reconnect_thread.join();
		m_reconnect_thread = {};
	}
	m_stop_reconnect.store(false);

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_state == OutputState::Idle)
			return;
	}

	obs_log(LOG_INFO, "OutputController [%s]: stopping", m_endpoint.name.c_str());

	if (m_output && obs_output_active(m_output)) {
		// AVANATRO-VERIFY: obs_output_stop is non-blocking — the "stop" signal
		// fires asynchronously.  We wait briefly for it to complete before
		// releasing encoders.  Alternatively, use obs_output_force_stop() which
		// is synchronous.  Using force_stop here for simplicity; a cleaner impl
		// would listen to the stop signal and release encoders in the callback.
		obs_output_force_stop(m_output);
	}

	do_release_encoders();
	set_state(OutputState::Idle);
}

/* -----------------------------------------------------------------------
 * do_release_encoders
 *
 * AVANATRO-VERIFY: Encoder release after output stop.
 * obs_encoder_release is safe after the output has stopped (output released
 * its internal ref on stop).  Confirm output truly has stopped before calling
 * (force_stop guarantees this).
 * ----------------------------------------------------------------------- */
void OutputController::do_release_encoders()
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
	self->set_state(OutputState::Live);
	obs_log(LOG_INFO, "OutputController [%s]: stream started (Live)",
	        self->m_endpoint.name.c_str());
}

void OutputController::on_stop_signal(void *data, calldata_t *cd)
{
	auto *self = static_cast<OutputController *>(data);
	// AVANATRO-VERIFY: calldata_int field name for stop error code.
	// In OBS 30 it is "code" (obs_output_t stop signal sends "code" int).
	int code = static_cast<int>(calldata_int(cd, "code"));
	self->handle_stop(code);
}

/* -----------------------------------------------------------------------
 * handle_stop — called from on_stop_signal (OBS signal thread)
 *
 * Decides whether to reconnect or go FailedHard based on the error code.
 * OBS_OUTPUT_SUCCESS = 0 means intentional stop (no reconnect).
 * Any non-zero code that isn't auth failure → reconnect.
 *
 * AVANATRO-VERIFY: OBS output stop codes.
 * Known codes from obs-output.h:
 *   OBS_OUTPUT_SUCCESS = 0
 *   OBS_OUTPUT_BAD_PATH = -3  (auth failure / invalid key — no reconnect)
 *   OBS_OUTPUT_CONNECT_FAILED = -8
 *   OBS_OUTPUT_DISCONNECTED = -9 (network drop — reconnect)
 * These may vary in OBS 31.x — check obs-output.h.
 * ----------------------------------------------------------------------- */
void OutputController::handle_stop(int code)
{
	/* OBS_OUTPUT_SUCCESS (0) and OBS_OUTPUT_BAD_PATH (-3) come from <obs-output.h>
	 * (which is included via OutputController.cpp's #include <obs.h> chain).
	 * We can NOT redefine them as constexpr — they are #define macros. */
	bool intentional = (code == OBS_OUTPUT_SUCCESS);
	bool auth_failure = (code == OBS_OUTPUT_BAD_PATH);

	if (intentional || auth_failure) {
		if (auth_failure) {
			const char *err = m_output ? obs_output_get_last_error(m_output) : nullptr;
			std::lock_guard<std::mutex> lock(m_mutex);
			m_last_error = err ? err : "Authentication failed — check stream key";
			m_state = OutputState::FailedHard;
			obs_log(LOG_ERROR, "OutputController [%s]: auth failure — %s",
			        m_endpoint.name.c_str(), m_last_error.c_str());
		} else {
			set_state(OutputState::Idle);
		}
		return;
	}

	/* Network disconnect — attempt reconnect */
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_state = OutputState::Reconnecting;
		obs_log(LOG_WARNING, "OutputController [%s]: disconnected (code=%d) — reconnecting",
		        m_endpoint.name.c_str(), code);
	}

	/* Launch reconnect thread if not already running */
	if (m_reconnect_thread.joinable())
		m_reconnect_thread.join();

	m_stop_reconnect.store(false);
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
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_state = OutputState::FailedHard;
				m_last_error = "Max reconnect attempts reached";
			}
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

		/* Release stale encoders before restarting */
		do_release_encoders();

		/* Attempt start — reuses existing m_output (service is still set) */
		m_video_enc = m_factory.create_video_encoder(m_endpoint, "smulti");
		m_audio_enc = m_factory.create_audio_encoder(m_endpoint, "smulti");

		if (!m_video_enc || !m_audio_enc) {
			obs_log(LOG_ERROR, "OutputController [%s]: encoder creation failed during reconnect",
			        m_endpoint.name.c_str());
			continue;
		}

		if (m_output) {
			obs_output_set_video_encoder(m_output, m_video_enc);
			obs_output_set_audio_encoder(m_output, m_audio_enc, 0);

			bool ok = obs_output_start(m_output);
			if (ok) {
				obs_log(LOG_INFO, "OutputController [%s]: reconnected successfully",
				        m_endpoint.name.c_str());
				/* on_start_signal will set state to Live */
				return;
			} else {
				const char *err = obs_output_get_last_error(m_output);
				obs_log(LOG_WARNING, "OutputController [%s]: reconnect attempt %d failed: %s",
				        m_endpoint.name.c_str(), m_reconnect_attempt,
				        err ? err : "(unknown)");
			}
		}
	}
}

} // namespace smulti
