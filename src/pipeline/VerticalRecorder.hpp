/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/VerticalRecord.hpp"
#include "EncoderFactory.hpp"
#include "VerticalEncodeSession.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// libobs forward declarations
struct obs_output;
typedef struct obs_output obs_output_t;
struct obs_encoder;
typedef struct obs_encoder obs_encoder_t;
struct obs_canvas;
typedef struct obs_canvas obs_canvas_t;
struct calldata;
typedef struct calldata calldata_t;

namespace smulti {

class ControllerReaper;
class OwnedCanvas;

enum class VerticalRecordState : int {
	Idle = 0,
	Recording = 1,
	Stopping = 2, ///< graceful stop requested, waiting for the output's "stop" signal
};

/**
 * Records the owned vertical canvas to a file (OBS adapter; the pure parts are core/VerticalRecord).
 *
 * One ffmpeg_muxer output (MKV) with a quality-based video encoder on the owned canvas's own video_t
 * (EncoderFactory::create_record_video_encoder) and AAC from the main mix track 1.
 *
 * Threading: every public member except state()/last_*() runs on the UI thread. The output's "stop"
 * signal arrives on an output thread and only posts finish() to the UI thread through `post_to_ui`
 * (tagged with the session number, so a late signal of an older session does nothing).
 *
 * Lifetime (same rules as OutputController): a strong canvas ref is held while recording, because it is
 * what keeps the encoder's video_t alive; it is released LAST, after the encoders and the output, on the
 * ControllerReaper thread (never the graphics thread). Forced stops (stop_now: canvas removed, scene
 * collection changing, scripting shutdown, exit, unload) are non-blocking: signal disconnected, session
 * detached, force_stop + releases enqueued on the reaper. The "stop" signal is disconnected outside the
 * mutex (libobs holds the signal mutex across the callback).
 *
 * After a graceful stop with code OBS_OUTPUT_SUCCESS the file is remuxed to MP4 on a worker thread when the
 * active profile has AutoRemux=true; shutdown() cancels a running remux and joins the worker.
 *
 * Runtime encode error (vertical_record_runtime_fallback): when an NVENC session stops with
 * OBS_OUTPUT_ENCODE_ERROR after a successful start, finish() keeps the partial file (remuxed like a
 * finished one), logs one warning and posts one restart with x264 CRF 20 into a NEW file (folder resolved
 * again, never C:). A second encode error ends the recording as before.
 */
class VerticalRecorder {
public:
	using UiPoster = std::function<void(std::function<void()>)>;

	VerticalRecorder(OwnedCanvas &canvas, ControllerReaper &reaper, UiPoster post_to_ui);
	~VerticalRecorder();
	VerticalRecorder(const VerticalRecorder &) = delete;
	VerticalRecorder &operator=(const VerticalRecorder &) = delete;

	/*
	 * ---- settings (persisted in <module config>/vertical-record.json with the hotkey bindings) ----
	 * The recorder is the only writer of that file; the vertical replay buffer reads its part
	 * (settings().replay, resolve_replay_folder()) and registers its hotkeys here (add_hotkey).
	 */
	VerticalRecordSettings settings() const;
	void set_settings(const VerticalRecordSettings &settings); ///< UI thread; writes the file
	void set_hotkey(size_t hotkey_id);                         ///< the record hotkey; loads the saved binding into it
	/** Persist `hotkey_id`'s binding under `key` of the settings file; loads the saved binding into it. */
	void add_hotkey(const std::string &key, size_t hotkey_id);
	void save_settings_file() const;

	/** The folder a start would record to, or why it is refused (reads the active profile). UI thread. */
	VerticalRecordFolder resolve_folder() const;
	/** The vertical replay buffer's clip folder (settings().replay.folder), same rules (never C:). UI thread. */
	VerticalRecordFolder resolve_replay_folder() const;

	/* ---- control (UI thread) ---- */
	bool start(bool with_main = false);
	void stop(); ///< graceful
	void toggle();
	void stop_now(const char *reason); ///< forced, non-blocking
	void on_main_recording_started();
	void on_main_recording_stopping();

	/** stop_now("unload"), cancel and join the remux worker. Idempotent. */
	void shutdown();

	/* ---- state (any thread) ---- */
	VerticalRecordState state() const;
	std::string last_file() const;
	std::string last_error() const;

	/** Called on the UI thread after every state change. */
	void set_state_listener(std::function<void()> listener);

private:
	using Session = VerticalSession;

	/**
	 * One start attempt with `choice`. On success the session (with `canvas`, whose ref then belongs to it)
	 * is published and recording; on failure everything it created is reaped, `canvas` stays the caller's
	 * and `error` says why. UI thread.
	 */
	bool try_start(const RecordEncoderChoice &choice, obs_canvas_t *canvas, video_t *video, const std::string &file,
		       bool with_main, std::string &error);
	/**
	 * start() body. `forced` replaces the encoder choice (runtime fallback restart, `runtime_restart` true:
	 * a further encode error of that session is not restarted again). UI thread.
	 */
	bool start_session(bool with_main, const std::optional<RecordEncoderChoice> &forced, bool runtime_restart);
	void restart_after_encode_error(const RecordEncoderChoice &choice, bool with_main); ///< UI thread
	static void on_output_stop(void *data, calldata_t *cd);
	void finish(std::uint64_t session, long long code);
	Session detach_locked();
	void reap(Session s, bool force);
	void fail(const std::string &reason);
	void notify();
	void queue_remux(const std::string &path);
	void remux_loop();

	OwnedCanvas &m_owned;
	ControllerReaper &m_reaper;
	UiPoster m_post;
	EncoderFactory m_factory;

	mutable std::mutex m_mutex;
	Session m_session;
	VerticalRecordState m_state = VerticalRecordState::Idle;
	bool m_started_with_main = false;
	bool m_remux_on_stop = false;
	std::string m_file;
	std::string m_encoder_id;          ///< video encoder type id of the running session
	bool m_runtime_restarted = false;  ///< the running session is the runtime fallback restart
	/**
	 * finish() posted a runtime fallback restart; stop/stop_now/shutdown cancel it, the main recording
	 * stopping cancels it only when the restarted session was started with the main recording.
	 */
	bool m_restart_pending = false;
	bool m_restart_with_main = false; ///< "started with main" of the session the pending restart continues
	std::string m_error;
	VerticalRecordSettings m_settings;
	std::vector<std::pair<std::string, size_t>> m_hotkeys; ///< settings-file key -> hotkey id
	std::atomic<std::uint64_t> m_session_id{0};
	std::function<void()> m_listener;
	bool m_shut_down = false;

	std::mutex m_remux_mutex;
	std::condition_variable m_remux_cv;
	std::deque<std::string> m_remux_queue;
	bool m_remux_quit = false;
	std::atomic<bool> m_remux_cancel{false};
	std::thread m_remux_thread;
};

} // namespace smulti
