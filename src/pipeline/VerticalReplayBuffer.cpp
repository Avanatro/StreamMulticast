/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalReplayBuffer.hpp"

#include "ControllerReaper.hpp"
#include "OwnedCanvas.hpp"
#include "VerticalRecorder.hpp"
#include "../plugin-support.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <utility>

namespace smulti {

namespace {

constexpr const char *kOutputName = "StreamMulticast Vertical Replay";

/** File name format of the buffer's own write; the "saved" handler renames it to the clip name. */
std::string replay_file_format()
{
	return std::string(kVerticalRecordFileStemFormat) + " replay";
}

} // namespace

VerticalReplayBuffer::VerticalReplayBuffer(OwnedCanvas &canvas, ControllerReaper &reaper,
					   VerticalRecorder &settings_owner, UiPoster post_to_ui)
	: m_owned(canvas),
	  m_reaper(reaper),
	  m_settings_owner(settings_owner),
	  m_post(std::move(post_to_ui))
{
}

VerticalReplayBuffer::~VerticalReplayBuffer()
{
	shutdown();
}

/* ---- state --------------------------------------------------------------- */

VerticalReplayState VerticalReplayBuffer::state() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_state;
}

std::string VerticalReplayBuffer::last_clip() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_last_clip;
}

std::string VerticalReplayBuffer::last_error() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_error;
}

void VerticalReplayBuffer::set_state_listener(std::function<void()> listener)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_listener = std::move(listener);
}

void VerticalReplayBuffer::notify()
{
	std::function<void()> listener;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		listener = m_listener;
	}
	if (listener)
		listener();
}

void VerticalReplayBuffer::fail(const std::string &reason)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_error = reason;
	}
	obs_log(LOG_WARNING, "vertical replay: %s", reason.c_str());
	notify();
}

/* ---- control ------------------------------------------------------------- */

bool VerticalReplayBuffer::start(bool with_main)
{
	return start_session(with_main, std::nullopt, false);
}

bool VerticalReplayBuffer::start_session(bool with_main, const std::optional<RecordEncoderChoice> &forced,
					 bool runtime_restart)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_shut_down || m_state != VerticalReplayState::Idle)
			return false;
	}

	const VerticalReplaySettings settings = clamp_vertical_replay_settings(m_settings_owner.settings().replay);
	const VerticalRecordFolder folder = m_settings_owner.resolve_replay_folder(); // never C:
	if (!folder.ok) {
		fail(folder.reason + " Not buffering.");
		return false;
	}

	std::string error;
	if (!ensure_vertical_folder(folder.folder, error)) {
		fail(error + " Not buffering.");
		return false;
	}

	/* Strong ref for the whole session: it keeps the canvas's video_t (the encoder's input) alive. */
	const VerticalCanvasVideo cv = acquire_vertical_canvas_video(m_owned, m_reaper);
	if (!cv.canvas) {
		fail(cv.error + " Not buffering.");
		return false;
	}
	obs_canvas_t *canvas = cv.canvas;
	video_t *video = cv.video;

	RecordEncoderChoice choice =
		forced ? *forced : choose_vertical_record_encoder(EncoderFactory::available_encoder_types());
	const bool started = start_with_encoder_fallback(
		"vertical replay", choice, error, [&](const RecordEncoderChoice &c, std::string &err) {
			return try_start(c, canvas, video, settings, folder.folder, with_main, err);
		});
	if (!started) {
		Session only_canvas; // try_start released everything it created; the canvas ref is still ours
		only_canvas.canvas = canvas;
		reap(only_canvas, false);
		fail(error + ". Not buffering.");
		return false;
	}
	{
		/* Before any posted finish() of this session can run (both on the UI thread). */
		std::lock_guard<std::mutex> lock(m_mutex);
		m_runtime_restarted = runtime_restart;
	}

	obs_log(LOG_INFO, "vertical replay: buffering %d s (max %d MB) into \"%s\" (%s %s %d%s)", settings.seconds,
		settings.max_size_mb, folder.folder.c_str(), choice.type_id.c_str(), choice.rate_control.c_str(),
		choice.quality, with_main ? ", with the main replay buffer" : "");
	notify();
	return true;
}

bool VerticalReplayBuffer::try_start(const RecordEncoderChoice &choice, obs_canvas_t *canvas, video_t *video,
				     const VerticalReplaySettings &settings, const std::string &folder, bool with_main,
				     std::string &error)
{
	Session s; // without the canvas until the start succeeded: a failed attempt must not release it
	create_vertical_encoders(m_factory, choice, kOutputName, video, s);

	obs_data_t *output_settings = obs_data_create();
	obs_data_set_int(output_settings, "max_time_sec", settings.seconds);
	obs_data_set_int(output_settings, "max_size_mb", settings.max_size_mb);
	obs_data_set_string(output_settings, "directory", folder.c_str());
	obs_data_set_string(output_settings, "format", replay_file_format().c_str());
	obs_data_set_string(output_settings, "extension", "mp4");
	obs_data_set_bool(output_settings, "allow_spaces", true);
	s.output = obs_output_create("replay_buffer", kOutputName, output_settings, nullptr);
	obs_data_release(output_settings);

	if (!s.video || !s.audio || !s.output) {
		reap(s, false);
		error = !s.video ? "Could not create the video encoder" : "Could not create the audio encoder or the replay output";
		return false;
	}
	obs_output_set_video_encoder(s.output, s.video);
	obs_output_set_audio_encoder(s.output, s.audio, 0);
	signal_handler_t *sh = obs_output_get_signal_handler(s.output);
	signal_handler_connect(sh, "stop", on_output_stop, this);
	signal_handler_connect(sh, "saved", on_output_saved, this);

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_session = s;
		m_session.canvas = canvas; // published with the session so a fast "stop" signal finds it complete
		m_state = VerticalReplayState::Buffering;
		m_started_with_main = with_main;
		m_folder = folder;
		m_pending = PendingClip{};
		m_error.clear();
		m_encoder_id = choice.type_id;
		m_restart_failed = false;
		m_session_id.fetch_add(1);
	}

	if (!obs_output_start(s.output)) {
		const char *err = obs_output_get_last_error(s.output);
		error = std::string("Could not start the replay buffer") + (err ? std::string(": ") + err : "");
		disconnect_signals(s.output);
		Session dead;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			dead = detach_locked();
		}
		dead.canvas = nullptr; // still owned by start(), which may retry with it
		reap(dead, false);
		return false;
	}
	return true;
}

void VerticalReplayBuffer::stop()
{
	obs_output_t *output = nullptr;
	std::uint64_t session = 0;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_restart_pending = false; // also when Idle: a stop cancels a posted runtime restart
		if (m_state != VerticalReplayState::Buffering)
			return;
		m_state = VerticalReplayState::Stopping;
		output = m_session.output;
		session = m_session_id.load();
	}
	obs_log(LOG_INFO, "vertical replay: stopping");
	notify();
	if (output && obs_output_active(output))
		obs_output_stop(output); // the "stop" signal posts finish()
	else
		finish(session, OBS_OUTPUT_SUCCESS);
}

void VerticalReplayBuffer::toggle()
{
	if (state() == VerticalReplayState::Idle)
		start(false);
	else
		stop();
}

bool VerticalReplayBuffer::save()
{
	obs_output_t *output = nullptr;
	obs_canvas_t *canvas = nullptr;
	bool restart_failed = false;
	std::string error;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_state == VerticalReplayState::Buffering) {
			output = m_session.output;
			canvas = m_session.canvas;
		}
		restart_failed = m_restart_failed;
		error = m_error;
	}
	if (!output || !obs_output_active(output)) {
		if (restart_failed)
			obs_log(LOG_WARNING,
				"vertical replay: NO CLIP SAVED, the buffer stopped after an encode error and its restart "
				"failed: %s",
				error.c_str());
		else
			obs_log(LOG_INFO, "vertical replay: not buffering, no clip saved");
		return false;
	}
	/* UI thread, like every teardown of the session: its canvas ref stays valid here. The scene and the date
	 * are the ones of the moment the clip was asked for. */
	PendingClip pending;
	pending.set = true;
	pending.date = local_time(std::time(nullptr));
	pending.scene = OwnedCanvas::active_scene_name(canvas);
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_pending = pending; // the buffer merges saves asked for before it wrote: the latest one names the clip
	}

	calldata_t cd = {};
	proc_handler_call(obs_output_get_proc_handler(output), "save", &cd);
	calldata_free(&cd);
	obs_log(LOG_INFO, "vertical replay: saving a clip (scene \"%s\")", pending.scene.c_str());
	return true;
}

void VerticalReplayBuffer::on_main_replay_started()
{
	if (m_settings_owner.settings().replay.enabled_with_main_replay && state() == VerticalReplayState::Idle)
		start(true);
}

void VerticalReplayBuffer::on_main_replay_stopping()
{
	bool with_main;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		with_main = m_started_with_main && m_state == VerticalReplayState::Buffering;
		if (m_restart_with_main)
			m_restart_pending = false; // a restart of a hand-started buffer stays pending
	}
	if (with_main)
		stop(); // a buffer started by hand keeps running
}

void VerticalReplayBuffer::stop_now(const char *reason)
{
	obs_output_t *output = nullptr;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_restart_pending = false; // also when Idle: a forced stop cancels a posted runtime restart
		if (m_state == VerticalReplayState::Idle)
			return;
		output = m_session.output;
	}
	/* Outside the mutex: libobs holds the signal mutex while a callback runs (a running "saved" rename
	 * finishes first). */
	disconnect_signals(output);
	Session s;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		s = detach_locked();
	}
	obs_log(LOG_INFO, "vertical replay: stopped (%s)", reason ? reason : "forced");
	reap(s, true);
	notify();
}

void VerticalReplayBuffer::shutdown()
{
	stop_now("unload");
	std::lock_guard<std::mutex> lock(m_mutex);
	m_shut_down = true;
	m_listener = nullptr;
}

/* ---- session teardown ---------------------------------------------------- */

void VerticalReplayBuffer::disconnect_signals(obs_output_t *output)
{
	if (!output)
		return;
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_disconnect(sh, "stop", on_output_stop, this);
	signal_handler_disconnect(sh, "saved", on_output_saved, this);
}

VerticalReplayBuffer::Session VerticalReplayBuffer::detach_locked()
{
	Session s = m_session;
	m_session = Session{};
	m_state = VerticalReplayState::Idle;
	m_started_with_main = false;
	m_pending = PendingClip{};
	m_session_id.fetch_add(1); // a pending finish() of this session becomes a no-op
	return s;
}

void VerticalReplayBuffer::reap(Session s, bool force)
{
	reap_vertical_session(m_reaper, s, force);
}

void VerticalReplayBuffer::on_output_stop(void *data, calldata_t *cd)
{
	/* Output thread, libobs signal mutex held: no lock of ours, only post to the UI thread. */
	auto *self = static_cast<VerticalReplayBuffer *>(data);
	const long long code = calldata_int(cd, "code");
	const std::uint64_t session = self->m_session_id.load();
	if (self->m_post)
		self->m_post([self, session, code]() { self->finish(session, code); });
}

void VerticalReplayBuffer::on_output_saved(void *data, calldata_t *)
{
	/* The output's mux thread, after the file is complete. Signal mutex held, so a stop_now/finish
	 * disconnect waits for this rename: the output stays alive meanwhile. */
	static_cast<VerticalReplayBuffer *>(data)->rename_saved_clip();
}

void VerticalReplayBuffer::rename_saved_clip()
{
	obs_output_t *output = nullptr;
	std::string folder;
	PendingClip pending;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		output = m_session.output;
		folder = m_folder;
		pending = m_pending;
		m_pending = PendingClip{};
	}
	if (!output || folder.empty())
		return;

	calldata_t cd = {};
	proc_handler_call(obs_output_get_proc_handler(output), "get_last_replay", &cd);
	const char *raw = calldata_string(&cd, "path");
	const std::string written = raw ? raw : "";
	calldata_free(&cd);
	if (written.empty()) {
		obs_log(LOG_WARNING, "vertical replay: a clip was written but its path is unknown");
		return;
	}

	if (!pending.set) {
		/* Saved without save() (the output's own hotkey): name it after now; the scene is unknown here. */
		pending.date = local_time(std::time(nullptr));
	}
	/* `folder` is the session's resolved clip folder (resolve_vertical_record_folder: never C:). The move never
	 * replaces a file: a name taken meanwhile gets the next " (N)". */
	const std::string stem = clip_file_stem(pending.date, pending.scene);
	std::string final_path = move_to_free_name(written, folder, stem, ".mp4");
	if (final_path.empty()) {
		obs_log(LOG_WARNING, "vertical replay: could not rename \"%s\" to \"%s\" in \"%s\", the clip keeps its name",
			written.c_str(), stem.c_str(), folder.c_str());
		final_path = written;
	} else {
		obs_log(LOG_INFO, "vertical replay: clip saved \"%s\"", final_path.c_str());
	}
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_last_clip = final_path;
	}
	if (m_post)
		m_post([this]() { notify(); });
}

void VerticalReplayBuffer::finish(std::uint64_t session, long long code)
{
	obs_output_t *output = nullptr;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (session != m_session_id.load() || m_state == VerticalReplayState::Idle)
			return; // an older session, or already torn down by stop_now()
		output = m_session.output;
	}
	disconnect_signals(output);

	const std::set<std::string> &available = EncoderFactory::available_encoder_types();
	std::string error;
	std::string failed_encoder;
	bool with_main = false;
	std::optional<RecordEncoderChoice> restart;
	Session s;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (session != m_session_id.load())
			return;
		with_main = m_started_with_main;
		failed_encoder = m_encoder_id;
		/* The recorder's rule: never after a requested stop (Stopping), only a buffer that was still running. */
		if (m_state == VerticalReplayState::Buffering)
			restart = vertical_record_runtime_fallback(code, m_encoder_id, m_runtime_restarted, available);
		m_restart_pending = restart.has_value();
		m_restart_with_main = with_main;
		if (code != OBS_OUTPUT_SUCCESS) {
			const char *err = output ? obs_output_get_last_error(output) : nullptr;
			m_error = "The replay buffer stopped with code " + std::to_string(code) +
				  (err ? std::string(": ") + err : "");
			if (restart)
				m_error += " (encode error with " + failed_encoder + ", restarting with " + restart->type_id + ")";
			error = m_error;
		}
		s = detach_locked();
	}
	reap(s, false);

	if (restart) {
		obs_log(LOG_WARNING,
			"vertical replay: encode error with %s, the buffered seconds are lost; restarting once with %s %s %d",
			failed_encoder.c_str(), restart->type_id.c_str(), restart->rate_control.c_str(), restart->quality);
		const RecordEncoderChoice choice = *restart;
		if (m_post)
			m_post([this, choice, failed_encoder, with_main]() {
				restart_after_encode_error(choice, failed_encoder, with_main);
			});
		notify();
		return;
	}

	if (code == OBS_OUTPUT_SUCCESS)
		obs_log(LOG_INFO, "vertical replay: stopped");
	else
		obs_log(LOG_WARNING, "vertical replay: %s", error.c_str());
	notify();
}

void VerticalReplayBuffer::restart_after_encode_error(const RecordEncoderChoice &choice,
						      const std::string &failed_encoder, bool with_main)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const bool pending = m_restart_pending;
		m_restart_pending = false;
		/* Cancelled by stop / stop_now / main replay buffer stopping / shutdown, or started again by hand. */
		if (!pending || m_shut_down || m_state != VerticalReplayState::Idle)
			return;
	}
	if (with_main && !obs_frontend_replay_buffer_active()) {
		obs_log(LOG_INFO, "vertical replay: not restarted, the main replay buffer has stopped meanwhile");
		return;
	}
	if (start_session(with_main, choice, true)) {
		/* Keep the failure visible in the dock while the restarted buffer runs. */
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_error = "Encode error with " + failed_encoder + ": restarted with " + choice.type_id +
				  ", clips start after the restart";
		}
		notify();
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_restart_failed = true;
		m_error = "Encode error with " + failed_encoder + ", the restart with " + choice.type_id +
			  " failed: " + m_error;
	}
	obs_log(LOG_WARNING, "vertical replay: the restart after an encode error with %s failed, no clips until the "
			     "buffer is started again",
		failed_encoder.c_str());
	notify();
}

} // namespace smulti
