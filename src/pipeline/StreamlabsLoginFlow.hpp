/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

/* Only compiled with -DSMULTI_STREAMLABS_TIKTOK=ON. */

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace smulti {

/**
 * StreamlabsLoginFlow — the Streamlabs login inside OBS (TASK-32), so the
 * external generator program is no longer needed. Requests, URL and parsing:
 * StreamlabsLogin (pure, tested). This adapter adds:
 *  - the PKCE randomness (BCryptGenRandom);
 *  - a loopback-only HTTP listener (Winsock, 127.0.0.1 on an OS-assigned
 *    port, never 0.0.0.0) on a worker thread. It accepts connections until a
 *    valid callback, the 300 s timeout or cancel; reads at most 8 KiB per
 *    request; answers other requests (favicon) with 404 and keeps waiting;
 *  - the browser (QDesktopServices::openUrl, UI thread);
 *  - the code exchange through StreamlabsClient (WinHTTP worker), then the
 *    token goes into StreamlabsTokenVault.
 *
 * Threading and lifetime: start(), cancel() and the callbacks run on the UI
 * thread. Results from the listener thread are posted with `post` (the owner
 * passes a poster bound to its own QObject, so a post to a destroyed owner is
 * dropped by Qt) and are dropped anyway once cancel() ran (shared alive flag,
 * as in StreamlabsClient). cancel() (also from the destructor and from
 * shutdown_all() at OBS exit/unload) makes the listener close its socket and
 * joins the thread within one 250 ms poll tick. The verifier, the code and
 * the token are wiped after use and never logged.
 */
class StreamlabsLoginFlow {
public:
	using UiPost = std::function<void(std::function<void()>)>;
	using StatusFn = std::function<void(const std::string &status)>;
	/** ok = a token is stored now; `message` is sanitized and safe to show. */
	using DoneFn = std::function<void(bool ok, const std::string &message)>;

	explicit StreamlabsLoginFlow(UiPost post);
	~StreamlabsLoginFlow();

	StreamlabsLoginFlow(const StreamlabsLoginFlow &) = delete;
	StreamlabsLoginFlow &operator=(const StreamlabsLoginFlow &) = delete;

	/**
	 * UI thread: open the listener and the browser. false (and a sanitized
	 * `error`) when the flow could not start; then no callback runs. While
	 * running, `status` reports progress and `done` runs exactly once unless
	 * cancel() comes first.
	 */
	bool start(StatusFn status, DoneFn done, std::string &error);
	/** UI thread, idempotent: abort the flow; no callback runs afterwards. */
	void cancel();
	bool running() const { return m_running; }

	/** UI thread: cancel every live flow (plugin-main at OBS exit and unload, before the client shuts down). */
	static void shutdown_all();

private:
	struct Outcome;
	void on_listener_done(const std::shared_ptr<Outcome> &outcome);
	void finish(bool ok, const std::string &message);
	void join_thread();

	UiPost m_post;
	std::shared_ptr<std::atomic<bool>> m_alive;  ///< false after cancel(): drops late results
	std::shared_ptr<std::atomic<bool>> m_cancel; ///< read by the listener thread
	std::thread m_thread;
	std::string m_verifier; ///< secret, UI thread only
	StatusFn m_status;
	DoneFn m_done;
	bool m_running = false;
};

/**
 * UI thread: read the Streamlabs Desktop login from the newest *.log in
 * %APPDATA%\slobs-client\Local Storage\leveldb (read-only, shared with a
 * running Streamlabs Desktop, at most 64 MiB) and store it in the vault.
 * `message` is sanitized: a success text or the reason it failed.
 */
bool streamlabs_desktop_import(std::string &message);

} // namespace smulti
