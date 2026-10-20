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
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

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
class VerticalRecorder;

enum class VerticalReplayState : int {
	Idle = 0,
	Buffering = 1,
	Stopping = 2, ///< graceful stop requested, waiting for the output's "stop" signal
};

/**
 * Replay buffer of the owned vertical canvas (OBS adapter; the pure parts are core/VerticalRecord): the last
 * 30..60 s of the canvas stay in memory, save() writes them as a ready 9:16 MP4 clip.
 *
 * One libobs `replay_buffer` output (max_time_sec, max_size_mb, directory = the resolved clip folder, never
 * C:) with its own quality-based video encoder on the owned canvas's video_t (choose_vertical_record_encoder,
 * NVENC HEVC CQP 20, x264 CRF 20 when NVENC is missing or refused at start) and AAC from the main mix track 1.
 * On the output's "saved" signal the written file (get_last_replay) is renamed to
 * "<folder>/<CCYY-MM-DD hh-mm-ss>-<V scene>.mp4" (clip_file_stem + vertical_record_file_path: never
 * overwrites), with the date and the active vertical scene (the destination while a transition runs) taken at
 * save() time.
 *
 * Settings and hotkey bindings live in the vertical recorder's settings file (`settings_owner`, the file's only
 * writer); settings are read at start().
 *
 * Threading and lifetime: the same rules as VerticalRecorder. Every public member except state()/last_*()
 * runs on the UI thread. "stop" only posts finish() (session-tagged); "saved" (the output's mux thread)
 * renames the file and posts the state notification. The strong canvas ref of the session is released LAST,
 * after the output and the encoders, on the ControllerReaper thread. stop_now (canvas removed, scene
 * collection changing, scripting shutdown, exit, unload) is non-blocking; the signals are disconnected outside
 * the mutex (libobs holds the signal mutex across the callback).
 *
 * Runtime encode error: the recorder's rule (vertical_record_runtime_fallback). When an NVENC session that was
 * still buffering stops with OBS_OUTPUT_ENCODE_ERROR, finish() keeps the error in last_error() (dock status),
 * logs one warning and posts one restart with x264 CRF 20 (the buffered seconds before the error are lost). A
 * pending restart is cancelled by stop / stop_now / shutdown and, for a buffer started with the main replay
 * buffer, by on_main_replay_stopping. A second encode error, any other error stop, or a failed restart ends the
 * buffer; save() then logs a warning with the reason instead of the plain "not buffering" line.
 */
class VerticalReplayBuffer {
public:
	using UiPoster = std::function<void(std::function<void()>)>;

	VerticalReplayBuffer(OwnedCanvas &canvas, ControllerReaper &reaper, VerticalRecorder &settings_owner,
			     UiPoster post_to_ui);
	~VerticalReplayBuffer();
	VerticalReplayBuffer(const VerticalReplayBuffer &) = delete;
	VerticalReplayBuffer &operator=(const VerticalReplayBuffer &) = delete;

	/* ---- control (UI thread) ---- */
	bool start(bool with_main = false);
	void stop(); ///< graceful
	void toggle();
	/** Write the buffer as a clip. False (logged) when the buffer is not running. */
	bool save();
	void stop_now(const char *reason); ///< forced, non-blocking
	void on_main_replay_started();
	void on_main_replay_stopping();

	/** stop_now("unload"); start() is refused afterwards. Idempotent. */
	void shutdown();

	/* ---- state (any thread) ---- */
	VerticalReplayState state() const;
	std::string last_clip() const;
	std::string last_error() const;

	/** Called on the UI thread after every state change and after a saved clip. */
	void set_state_listener(std::function<void()> listener);

private:
	using Session = VerticalSession;

	/** Name parts taken at save() time; the "saved" signal consumes them (latest save wins). */
	struct PendingClip {
		bool set = false;
		std::tm date = {};
		std::string scene;
	};

	bool start_session(bool with_main, const std::optional<RecordEncoderChoice> &forced, bool runtime_restart);
	void restart_after_encode_error(const RecordEncoderChoice &choice, const std::string &failed_encoder,
					bool with_main); ///< UI thread
	bool try_start(const RecordEncoderChoice &choice, obs_canvas_t *canvas, video_t *video,
		       const VerticalReplaySettings &settings, const std::string &folder, bool with_main,
		       std::string &error);
	static void on_output_stop(void *data, calldata_t *cd);
	static void on_output_saved(void *data, calldata_t *cd);
	void disconnect_signals(obs_output_t *output);
	void rename_saved_clip();
	void finish(std::uint64_t session, long long code);
	Session detach_locked();
	void reap(Session s, bool force);
	void fail(const std::string &reason);
	void notify();

	OwnedCanvas &m_owned;
	ControllerReaper &m_reaper;
	VerticalRecorder &m_settings_owner;
	UiPoster m_post;
	EncoderFactory m_factory;

	mutable std::mutex m_mutex;
	Session m_session;
	VerticalReplayState m_state = VerticalReplayState::Idle;
	bool m_started_with_main = false;
	std::string m_folder; ///< resolved clip folder of the running session
	PendingClip m_pending;
	std::string m_last_clip;
	std::string m_error;
	std::string m_encoder_id;          ///< video encoder type id of the running session
	bool m_runtime_restarted = false;  ///< the running session is the runtime fallback restart
	/* A runtime restart posted by finish(): cleared by every stop path, consumed by restart_after_encode_error. */
	bool m_restart_pending = false;
	bool m_restart_with_main = false;  ///< "started with main" of the session the pending restart continues
	/** Ended by an error without a restart (or the restart failed); save() warns until the next start. */
	bool m_stopped_on_error = false;
	std::atomic<std::uint64_t> m_session_id{0};
	std::function<void()> m_listener;
	bool m_shut_down = false;
};

} // namespace smulti
