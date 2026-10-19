/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

/* Only compiled with -DSMULTI_STREAMLABS_TIKTOK=ON. */

#include "../core/StreamlabsTikTok.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace smulti {

/**
 * StreamlabsClient — WinHTTP adapter and session bookkeeping for the
 * unofficial Streamlabs TikTok API (requests and parsing: StreamlabsTikTok).
 *
 * Threading: every network call runs on one worker thread (FIFO, so an end
 * queued before a start always runs first). Results are handed to the UI
 * thread through the `post_to_ui` function given at construction (queued
 * QMetaObject::invokeMethod in plugin-main). A shared alive flag drops every
 * result that arrives after shutdown_and_end_blocking(); that call cancels the
 * in-flight request, joins the worker and then ends any open session
 * synchronously (bounded to 5 s per session). It must run before the UI post
 * context is deleted.
 *
 * Sessions: one Streamlabs account has one TikTok live session. The open
 * session id is kept in memory together with the endpoint that owns it and in
 * obs_module_config_path("streamlabs_tiktok_session.txt") (id only, never the
 * key), so a session left open by a crash is ended before the next start.
 *
 * TLS only (https URLs), 15 s timeouts, 5 s for end. Never logs the token,
 * the key, the RTMP URL or a response body.
 */
class StreamlabsClient {
public:
	using UiPost = std::function<void(std::function<void()>)>;
	using ResultFn = std::function<void(const StreamlabsResult &)>;
	/** UI thread. Return true when the caller took the session; false makes the client end it. */
	using AdoptFn = std::function<bool(const StreamlabsResult &)>;

	explicit StreamlabsClient(UiPost post_to_ui);
	~StreamlabsClient();

	StreamlabsClient(const StreamlabsClient &) = delete;
	StreamlabsClient &operator=(const StreamlabsClient &) = delete;

	/** UI thread: account status ("Connected as @user"). */
	void info(ResultFn done);
	/** UI thread: category search, result always ends with "Other". */
	void search(const std::string &query, ResultFn done);

	/**
	 * UI thread: pre-start key fetch for `endpoint_id`. Ends a stale or
	 * still-open session of this endpoint first, then starts a new one. `adopt`
	 * runs on the UI thread with the result (also on failure). Fails without a
	 * network call when no token is stored or another endpoint owns the open
	 * session.
	 */
	void start_session(const std::string &endpoint_id, const std::string &endpoint_name, const std::string &title,
			   const std::string &category_id, AdoptFn adopt);

	/** Any thread: end the session `endpoint_id` owns (no-op otherwise). Asynchronous, at most once. */
	void end_session(const std::string &endpoint_id);

	/** UI thread, idempotent: cancel, join, then end every open session synchronously (5 s each). */
	void shutdown_and_end_blocking();

private:
	/** Cancels the one WinHTTP request registered in it by closing its handle. */
	struct CancelSlot {
		std::mutex mutex;
		void *request = nullptr; // HINTERNET
		bool cancelled = false;
		void cancel();
	};

	struct HttpResult {
		bool transport_ok = false;
		int status = 0;
		std::string body;
		std::string transport_error; // sanitized
	};

	HttpResult perform(const StreamlabsRequest &req, const std::string &token, int timeout_ms, CancelSlot &slot);
	StreamlabsResult end_now(const std::string &token, const std::string &session_id, CancelSlot &slot);
	void end_blocking_bounded(const std::string &token, const std::string &session_id);

	void enqueue(std::function<void()> job);
	void worker_loop();
	void post(std::function<void()> fn);

	UiPost m_post;
	std::shared_ptr<std::atomic<bool>> m_alive;

	std::mutex m_mutex; // guards the queue and the session fields
	std::condition_variable m_cv;
	std::deque<std::function<void()>> m_jobs;
	bool m_stopping = false;
	bool m_shut_down = false;
	std::thread m_worker;
	CancelSlot m_worker_slot;

	std::string m_open_id;    ///< open session id ("" = none)
	std::string m_open_owner; ///< endpoint id that owns m_open_id
	std::string m_open_name;  ///< endpoint name for log lines
	std::vector<std::string> m_closing_ids; ///< end queued, not yet confirmed
};

/** Process-wide instance (set by plugin-main while loaded, nullptr otherwise). */
StreamlabsClient *streamlabs_client();
void set_streamlabs_client(StreamlabsClient *client);

} // namespace smulti
