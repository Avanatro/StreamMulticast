/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalRecorder.hpp"

#include "ControllerReaper.hpp"
#include "OwnedCanvas.hpp"
#include "../plugin-support.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <media-io/media-remux.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <ctime>
#include <optional>
#include <set>
#include <utility>

namespace smulti {

namespace {

constexpr const char *kSettingsFile = "vertical-record.json";
constexpr const char *kHotkeyKey = "hotkey";
constexpr const char *kOutputName = "StreamMulticast Vertical Recording";
constexpr size_t kNoHotkey = static_cast<size_t>(-1);

std::string settings_path()
{
	char *raw = obs_module_get_config_path(obs_current_module(), kSettingsFile);
	std::string path = raw ? raw : "";
	bfree(raw);
	return path;
}

/** Main recording path of the active profile ("" when unknown). Frontend API: UI thread. */
std::string main_record_path()
{
	char *raw = obs_frontend_get_current_record_output_path();
	std::string path = raw ? raw : "";
	bfree(raw);
	return path;
}

bool profile_auto_remux()
{
	config_t *profile = obs_frontend_get_profile_config();
	return profile && config_get_bool(profile, "Video", "AutoRemux");
}

bool remux_progress(void *param, float)
{
	/* returning false aborts the remux job */
	return !static_cast<std::atomic<bool> *>(param)->load();
}

} // namespace

VerticalRecorder::VerticalRecorder(OwnedCanvas &canvas, ControllerReaper &reaper, UiPoster post_to_ui)
	: m_owned(canvas),
	  m_reaper(reaper),
	  m_post(std::move(post_to_ui))
{
	const std::string path = settings_path();
	obs_data_t *data = path.empty() ? nullptr : obs_data_create_from_json_file(path.c_str());
	m_settings = vertical_record_settings_load(data);
	obs_data_release(data);
}

VerticalRecorder::~VerticalRecorder()
{
	shutdown();
}

/* ---- settings ------------------------------------------------------------ */

VerticalRecordSettings VerticalRecorder::settings() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_settings;
}

void VerticalRecorder::set_settings(const VerticalRecordSettings &settings)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_settings = settings;
	}
	save_settings_file();
}

void VerticalRecorder::set_hotkey(size_t hotkey_id)
{
	add_hotkey(kHotkeyKey, hotkey_id);
}

void VerticalRecorder::add_hotkey(const std::string &key, size_t hotkey_id)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		bool replaced = false;
		for (auto &entry : m_hotkeys) {
			if (entry.first == key) {
				entry.second = hotkey_id;
				replaced = true;
			}
		}
		if (!replaced)
			m_hotkeys.emplace_back(key, hotkey_id);
	}
	const std::string path = settings_path();
	obs_data_t *data = path.empty() ? nullptr : obs_data_create_from_json_file(path.c_str());
	if (data) {
		obs_data_array_t *binding = obs_data_get_array(data, key.c_str());
		if (binding)
			obs_hotkey_load(hotkey_id, binding);
		obs_data_array_release(binding);
		obs_data_release(data);
	}
}

void VerticalRecorder::save_settings_file() const
{
	const std::string path = settings_path();
	if (path.empty())
		return;
	VerticalRecordSettings settings;
	std::vector<std::pair<std::string, size_t>> hotkeys;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		settings = m_settings;
		hotkeys = m_hotkeys;
	}
	obs_data_t *data = obs_data_create();
	vertical_record_settings_save(data, settings);
	for (const auto &[key, hotkey] : hotkeys) {
		if (hotkey == kNoHotkey)
			continue;
		obs_data_array_t *binding = obs_hotkey_save(hotkey);
		obs_data_set_array(data, key.c_str(), binding);
		obs_data_array_release(binding);
	}
	if (!obs_data_save_json_safe(data, path.c_str(), "tmp", "bak"))
		obs_log(LOG_WARNING, "vertical recording: could not write %s", path.c_str());
	obs_data_release(data);
}

VerticalRecordFolder VerticalRecorder::resolve_folder() const
{
	return resolve_vertical_record_folder(main_record_path(), settings().folder);
}

VerticalRecordFolder VerticalRecorder::resolve_replay_folder() const
{
	return resolve_vertical_record_folder(main_record_path(), settings().replay.folder);
}

/* ---- state --------------------------------------------------------------- */

VerticalRecordState VerticalRecorder::state() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_state;
}

std::string VerticalRecorder::last_file() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_file;
}

std::string VerticalRecorder::last_error() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_error;
}

void VerticalRecorder::set_state_listener(std::function<void()> listener)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_listener = std::move(listener);
}

void VerticalRecorder::notify()
{
	std::function<void()> listener;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		listener = m_listener;
	}
	if (listener)
		listener();
}

void VerticalRecorder::fail(const std::string &reason)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_error = reason;
	}
	obs_log(LOG_WARNING, "vertical recording: %s", reason.c_str());
	notify();
}

/* ---- control ------------------------------------------------------------- */

bool VerticalRecorder::start(bool with_main)
{
	return start_session(with_main, std::nullopt, false);
}

bool VerticalRecorder::start_session(bool with_main, const std::optional<RecordEncoderChoice> &forced,
				     bool runtime_restart)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_shut_down || m_state != VerticalRecordState::Idle)
			return false;
	}

	const VerticalRecordFolder folder = resolve_folder();
	if (!folder.ok) {
		fail(folder.reason + " Not recording.");
		return false;
	}
	std::string error;
	if (!ensure_vertical_folder(folder.folder, error)) {
		fail(error + " Not recording.");
		return false;
	}

	/* Strong ref for the whole session: it keeps the canvas's video_t (the encoder's input) alive. */
	const VerticalCanvasVideo cv = acquire_vertical_canvas_video(m_owned, m_reaper);
	if (!cv.canvas) {
		fail(cv.error + " Not recording.");
		return false;
	}
	obs_canvas_t *canvas = cv.canvas;
	video_t *video = cv.video;

	const std::string file = vertical_record_file_path(
		folder.folder, format_record_file_stem(kVerticalRecordFileStemFormat, local_time(std::time(nullptr))),
		[](const std::string &p) { return os_file_exists(p.c_str()); });

	RecordEncoderChoice choice =
		forced ? *forced : choose_vertical_record_encoder(EncoderFactory::available_encoder_types());
	const bool started = start_with_encoder_fallback(
		"vertical recording", choice, error, [&](const RecordEncoderChoice &c, std::string &err) {
			return try_start(c, canvas, video, file, with_main, err);
		});
	if (!started) {
		Session only_canvas; // try_start released everything it created; the canvas ref is still ours
		only_canvas.canvas = canvas;
		reap(only_canvas, false);
		fail(error + ". Not recording.");
		return false;
	}
	{
		/* Before any posted finish() of this session can run (both on the UI thread). */
		std::lock_guard<std::mutex> lock(m_mutex);
		m_runtime_restarted = runtime_restart;
	}

	obs_log(LOG_INFO, "vertical recording: started \"%s\" (%s %s %d%s)", file.c_str(), choice.type_id.c_str(),
		choice.rate_control.c_str(), choice.quality, with_main ? ", with the main recording" : "");
	notify();
	return true;
}

bool VerticalRecorder::try_start(const RecordEncoderChoice &choice, obs_canvas_t *canvas, video_t *video,
				 const std::string &file, bool with_main, std::string &error)
{
	Session s; // without the canvas until the start succeeded: a failed attempt must not release it
	create_vertical_encoders(m_factory, choice, kOutputName, video, s);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", file.c_str());
	s.output = obs_output_create("ffmpeg_muxer", kOutputName, settings, nullptr);
	obs_data_release(settings);

	if (!s.video || !s.audio || !s.output) {
		reap(s, false);
		error = !s.video ? "Could not create the video encoder" : "Could not create the recording audio encoder or output";
		return false;
	}
	obs_output_set_video_encoder(s.output, s.video);
	obs_output_set_audio_encoder(s.output, s.audio, 0);
	signal_handler_connect(obs_output_get_signal_handler(s.output), "stop", on_output_stop, this);

	const bool remux = profile_auto_remux();
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_session = s;
		m_session.canvas = canvas; // published with the session so a fast "stop" signal finds it complete
		m_state = VerticalRecordState::Recording;
		m_started_with_main = with_main;
		m_remux_on_stop = remux;
		m_file = file;
		m_encoder_id = choice.type_id;
		m_error.clear();
		m_session_id.fetch_add(1);
	}

	if (!obs_output_start(s.output)) {
		const char *err = obs_output_get_last_error(s.output);
		error = std::string("Could not start the recording") + (err ? std::string(": ") + err : "");
		signal_handler_disconnect(obs_output_get_signal_handler(s.output), "stop", on_output_stop, this);
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

void VerticalRecorder::stop()
{
	obs_output_t *output = nullptr;
	std::uint64_t session = 0;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_restart_pending = false; // also when Idle: a stop cancels a posted runtime restart
		if (m_state != VerticalRecordState::Recording)
			return;
		m_state = VerticalRecordState::Stopping;
		output = m_session.output;
		session = m_session_id.load();
	}
	obs_log(LOG_INFO, "vertical recording: stopping");
	notify();
	if (output && obs_output_active(output))
		obs_output_stop(output); // the "stop" signal posts finish()
	else
		finish(session, OBS_OUTPUT_SUCCESS);
}

void VerticalRecorder::toggle()
{
	if (state() == VerticalRecordState::Idle)
		start(false);
	else
		stop();
}

void VerticalRecorder::on_main_recording_started()
{
	if (settings().record_with_main && state() == VerticalRecordState::Idle)
		start(true);
}

void VerticalRecorder::on_main_recording_stopping()
{
	bool with_main;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		/* The main recording is stopping: no runtime restart of a session started with it; the restart of a
		 * vertical recording started by hand stays pending, like the recording itself would keep running. */
		if (m_restart_with_main)
			m_restart_pending = false;
		with_main = m_started_with_main && m_state == VerticalRecordState::Recording;
	}
	if (with_main)
		stop(); // a vertical recording started by hand keeps running
}

void VerticalRecorder::stop_now(const char *reason)
{
	obs_output_t *output = nullptr;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_restart_pending = false; // also when Idle: a forced stop cancels a posted runtime restart
		if (m_state == VerticalRecordState::Idle)
			return;
		output = m_session.output;
	}
	/* Outside the mutex: libobs holds the signal mutex while on_output_stop runs. */
	if (output)
		signal_handler_disconnect(obs_output_get_signal_handler(output), "stop", on_output_stop, this);
	Session s;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		s = detach_locked();
	}
	obs_log(LOG_INFO, "vertical recording: stopped (%s)", reason ? reason : "forced");
	reap(s, true);
	notify();
}

void VerticalRecorder::shutdown()
{
	stop_now("unload");
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_shut_down = true;
		m_restart_pending = false;
		m_listener = nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(m_remux_mutex);
		m_remux_quit = true;
		m_remux_cancel = true;
	}
	m_remux_cv.notify_all();
	if (m_remux_thread.joinable())
		m_remux_thread.join();
}

/* ---- session teardown ---------------------------------------------------- */

VerticalRecorder::Session VerticalRecorder::detach_locked()
{
	Session s = m_session;
	m_session = Session{};
	m_state = VerticalRecordState::Idle;
	m_started_with_main = false;
	m_session_id.fetch_add(1); // a pending finish() of this session becomes a no-op
	return s;
}

void VerticalRecorder::reap(Session s, bool force)
{
	reap_vertical_session(m_reaper, s, force);
}

void VerticalRecorder::on_output_stop(void *data, calldata_t *cd)
{
	/* Output thread, libobs signal mutex held: no lock of ours, only post to the UI thread. */
	auto *self = static_cast<VerticalRecorder *>(data);
	const long long code = calldata_int(cd, "code");
	const std::uint64_t session = self->m_session_id.load();
	if (self->m_post)
		self->m_post([self, session, code]() { self->finish(session, code); });
}

void VerticalRecorder::finish(std::uint64_t session, long long code)
{
	obs_output_t *output = nullptr;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (session != m_session_id.load() || m_state == VerticalRecordState::Idle)
			return; // an older session, or already torn down by stop_now()
		output = m_session.output;
	}
	if (output)
		signal_handler_disconnect(obs_output_get_signal_handler(output), "stop", on_output_stop, this);

	const std::set<std::string> &available = EncoderFactory::available_encoder_types();
	std::string file;
	bool remux;
	bool with_main;
	std::string error;
	std::string failed_encoder;
	std::optional<RecordEncoderChoice> restart;
	Session s;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (session != m_session_id.load())
			return;
		file = m_file;
		remux = m_remux_on_stop;
		with_main = m_started_with_main;
		failed_encoder = m_encoder_id;
		/* Never after a requested stop (Stopping): only a session that was still recording restarts. */
		if (m_state == VerticalRecordState::Recording)
			restart = vertical_record_runtime_fallback(code, m_encoder_id, m_runtime_restarted, available);
		m_restart_pending = restart.has_value();
		m_restart_with_main = with_main;
		if (code != OBS_OUTPUT_SUCCESS) {
			const char *err = output ? obs_output_get_last_error(output) : nullptr;
			m_error = "The recording stopped with code " + std::to_string(code) +
				  (err ? std::string(": ") + err : "");
			error = m_error;
		}
		s = detach_locked();
	}
	reap(s, false);

	if (restart) {
		/* Runtime NVENC failure after a successful start: keep the partial file, continue once with x264. */
		obs_log(LOG_WARNING,
			"vertical recording: encode error with %s, the partial file \"%s\" is kept; restarting with %s %s %d "
			"in a new file",
			failed_encoder.c_str(), file.c_str(), restart->type_id.c_str(), restart->rate_control.c_str(),
			restart->quality);
		if (remux)
			queue_remux(file);
		const RecordEncoderChoice choice = *restart;
		if (m_post)
			m_post([this, choice, with_main]() { restart_after_encode_error(choice, with_main); });
		notify();
		return;
	}

	if (code == OBS_OUTPUT_SUCCESS) {
		obs_log(LOG_INFO, "vertical recording: saved \"%s\"", file.c_str());
		if (remux)
			queue_remux(file);
	} else {
		obs_log(LOG_WARNING, "vertical recording: %s", error.c_str());
	}
	notify();
}

void VerticalRecorder::restart_after_encode_error(const RecordEncoderChoice &choice, bool with_main)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const bool pending = m_restart_pending;
		m_restart_pending = false;
		/* Cancelled by stop / stop_now / main recording stopping / shutdown, or started again by hand. */
		if (!pending || m_shut_down || m_state != VerticalRecordState::Idle)
			return;
	}
	if (with_main && !obs_frontend_recording_active()) {
		obs_log(LOG_INFO, "vertical recording: not restarted, the main recording has stopped meanwhile");
		return;
	}
	start_session(with_main, choice, true); // new file name, folder resolved again (never C:)
}

/* ---- remux worker -------------------------------------------------------- */

void VerticalRecorder::queue_remux(const std::string &path)
{
	{
		std::lock_guard<std::mutex> lock(m_remux_mutex);
		if (m_remux_quit)
			return;
		m_remux_queue.push_back(path);
		if (!m_remux_thread.joinable())
			m_remux_thread = std::thread(&VerticalRecorder::remux_loop, this);
	}
	m_remux_cv.notify_one();
}

void VerticalRecorder::remux_loop()
{
	for (;;) {
		std::string in;
		{
			std::unique_lock<std::mutex> lock(m_remux_mutex);
			m_remux_cv.wait(lock, [this] { return m_remux_quit || !m_remux_queue.empty(); });
			if (m_remux_quit)
				return;
			in = std::move(m_remux_queue.front());
			m_remux_queue.pop_front();
		}
		const std::string out = remux_target_path(in);
		media_remux_job_t job = nullptr;
		bool ok = media_remux_job_create(&job, in.c_str(), out.c_str());
		if (ok) {
			ok = media_remux_job_process(job, remux_progress, &m_remux_cancel);
			media_remux_job_destroy(job);
		}
		if (ok) {
			obs_log(LOG_INFO, "vertical recording: remuxed to \"%s\"", out.c_str());
		} else if (m_remux_cancel.load()) {
			/* Cancelled at shutdown: the MP4 is truncated, the MKV next to it is complete. */
			const bool removed = os_unlink(out.c_str()) == 0;
			obs_log(LOG_WARNING, "vertical recording: remux of \"%s\" cancelled, %s the partial \"%s\"",
				in.c_str(), removed ? "deleted" : "could not delete", out.c_str());
		} else {
			obs_log(LOG_WARNING, "vertical recording: remux of \"%s\" failed", in.c_str());
		}
	}
}

} // namespace smulti
