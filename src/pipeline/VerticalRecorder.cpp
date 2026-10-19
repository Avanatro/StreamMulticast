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
#include <utility>

namespace smulti {

namespace {

constexpr const char *kSettingsFile = "vertical-record.json";
constexpr const char *kHotkeyKey = "hotkey";
constexpr const char *kOutputName = "StreamMulticast Vertical Recording";
constexpr int kAudioBitrateKbps = 160; // OBS default for recording track 1
constexpr size_t kMainMixTrack1 = 0;
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

std::tm local_now()
{
	const std::time_t now = std::time(nullptr);
	std::tm t = {};
#ifdef _WIN32
	localtime_s(&t, &now);
#else
	localtime_r(&now, &t);
#endif
	return t;
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
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_hotkey = hotkey_id;
	}
	const std::string path = settings_path();
	obs_data_t *data = path.empty() ? nullptr : obs_data_create_from_json_file(path.c_str());
	if (data) {
		obs_data_array_t *binding = obs_data_get_array(data, kHotkeyKey);
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
	size_t hotkey;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		settings = m_settings;
		hotkey = m_hotkey;
	}
	obs_data_t *data = obs_data_create();
	vertical_record_settings_save(data, settings);
	if (hotkey != kNoHotkey) {
		obs_data_array_t *binding = obs_hotkey_save(hotkey);
		obs_data_set_array(data, kHotkeyKey, binding);
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
	if (os_mkdirs(folder.folder.c_str()) == MKDIR_ERROR) {
		fail("Could not create the folder \"" + folder.folder + "\". Not recording.");
		return false;
	}

	/* Strong ref for the whole session: it keeps the canvas's video_t (the encoder's input) alive. */
	Session s;
	s.canvas = m_owned.get_ref();
	if (!s.canvas) {
		fail("The vertical canvas is not available. Not recording.");
		return false;
	}
	video_t *video = obs_canvas_get_video(s.canvas);
	if (!video) {
		reap(s, false);
		fail("The vertical canvas has no video yet. Not recording.");
		return false;
	}

	const std::string file = vertical_record_file_path(
		folder.folder, format_record_file_stem(kVerticalRecordFileStemFormat, local_now()),
		[](const std::string &p) { return os_file_exists(p.c_str()); });

	const RecordEncoderChoice choice = choose_vertical_record_encoder(EncoderFactory::available_encoder_types());
	s.video = m_factory.create_record_video_encoder(choice, std::string(kOutputName) + " video", video);
	s.audio = m_factory.create_aac_encoder(std::string(kOutputName) + " audio", kAudioBitrateKbps, kMainMixTrack1);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", file.c_str());
	s.output = obs_output_create("ffmpeg_muxer", kOutputName, settings, nullptr);
	obs_data_release(settings);

	if (!s.video || !s.audio || !s.output) {
		reap(s, false);
		fail("Could not create the recording encoders or output. Not recording.");
		return false;
	}
	obs_output_set_video_encoder(s.output, s.video);
	obs_output_set_audio_encoder(s.output, s.audio, 0);
	signal_handler_connect(obs_output_get_signal_handler(s.output), "stop", on_output_stop, this);

	const bool remux = profile_auto_remux();
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_session = s;
		m_state = VerticalRecordState::Recording;
		m_started_with_main = with_main;
		m_remux_on_stop = remux;
		m_file = file;
		m_error.clear();
		m_session_id.fetch_add(1);
	}

	if (!obs_output_start(s.output)) {
		const char *err = obs_output_get_last_error(s.output);
		const std::string reason = std::string("Could not start the recording") + (err ? std::string(": ") + err : "");
		signal_handler_disconnect(obs_output_get_signal_handler(s.output), "stop", on_output_stop, this);
		Session dead;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			dead = detach_locked();
		}
		reap(dead, false);
		fail(reason);
		return false;
	}

	obs_log(LOG_INFO, "vertical recording: started \"%s\" (%s %s %d%s)", file.c_str(), choice.type_id.c_str(),
		choice.rate_control.c_str(), choice.quality, with_main ? ", with the main recording" : "");
	notify();
	return true;
}

void VerticalRecorder::stop()
{
	obs_output_t *output = nullptr;
	std::uint64_t session = 0;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
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
	if (!s.output && !s.video && !s.audio && !s.canvas)
		return;
	/* Off the UI thread, and never on the graphics thread: the canvas ref is released LAST, after the
	 * encoders that read its video_t and the output that holds them. */
	m_reaper.enqueue([s, force]() {
		if (s.output) {
			if (force)
				obs_output_force_stop(s.output);
			obs_output_release(s.output);
		}
		if (s.video)
			obs_encoder_release(s.video);
		if (s.audio)
			obs_encoder_release(s.audio);
		if (s.canvas)
			obs_canvas_release(s.canvas);
	});
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

	std::string file;
	bool remux;
	std::string error;
	Session s;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (session != m_session_id.load())
			return;
		file = m_file;
		remux = m_remux_on_stop;
		if (code != OBS_OUTPUT_SUCCESS) {
			const char *err = output ? obs_output_get_last_error(output) : nullptr;
			m_error = "The recording stopped with code " + std::to_string(code) +
				  (err ? std::string(": ") + err : "");
			error = m_error;
		}
		s = detach_locked();
	}
	reap(s, false);

	if (code == OBS_OUTPUT_SUCCESS) {
		obs_log(LOG_INFO, "vertical recording: saved \"%s\"", file.c_str());
		if (remux)
			queue_remux(file);
	} else {
		obs_log(LOG_WARNING, "vertical recording: %s", error.c_str());
	}
	notify();
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
		if (ok)
			obs_log(LOG_INFO, "vertical recording: remuxed to \"%s\"", out.c_str());
		else
			obs_log(LOG_WARNING, "vertical recording: remux of \"%s\" failed or was cancelled", in.c_str());
	}
}

} // namespace smulti
