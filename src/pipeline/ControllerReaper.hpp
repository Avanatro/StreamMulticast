/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace smulti {

class OutputController;

/**
 * ControllerReaper — moves OutputController teardown off the Qt UI thread.
 *
 * Problem: ~OutputController() (via shutdown_blocking()) ends in
 * obs_output_release(), which blocks on os_event_wait(output->stopping_event)
 * until the output's internal send thread has fully exited (see obs-output.c
 * obs_output_destroy()).  If that thread is stuck in a socket call (a stalled
 * RTMP server, a dead TCP connection that hasn't timed out yet), this wait is
 * effectively indefinite.  EndpointRegistry::remove()/update() used to reach
 * this call synchronously — partly while holding EndpointRegistry's own
 * mutex — so one hung endpoint could freeze the whole dock (and, via that
 * mutex, every other endpoint's UI interaction too).
 *
 * Fix: EndpointRegistry extracts the OutputController shared_ptr from its map
 * under its own lock, releases that lock, then hands the controller to this
 * reaper.  A single dedicated worker thread drains a FIFO queue of jobs — off
 * the Qt thread.
 *
 * Job model (fix-round 1, 2026-07-07): the queue holds `std::function<void()>`
 * jobs, not just controllers.  This lets `OutputController::stop()` (see its
 * doc comment) also detach a live session (output + encoders + reconnect
 * thread) onto this same reaper instead of blocking the caller — it is no
 * longer only "the place ~OutputController's teardown runs".  The original
 * controller-queue use is kept as a convenience overload:
 * `enqueue(shared_ptr<OutputController>)` just wraps
 * `[c = std::move(controller)] { c->shutdown_blocking(); }` and enqueues that.
 *
 * Deliberately NOT a generic thread pool: one thread, FIFO order, no futures,
 * no cancellation beyond process shutdown.  If parallelism is ever needed,
 * that is a new component, not a reason to generalize this one.
 *
 * Shutdown discipline: shutdown() signals the worker, drains the queue, and
 * then BLOCK-JOINS the worker thread.  It must never detach — a detached
 * thread continues running inside this plugin's code segment even after the
 * OS unloads the DLL, which is a guaranteed use-after-free the moment it next
 * touches any of this translation unit's code or data.
 */
class ControllerReaper {
public:
	ControllerReaper();
	~ControllerReaper();

	/* Non-copyable, non-movable (owns a mutex, condvar and thread) */
	ControllerReaper(const ControllerReaper &) = delete;
	ControllerReaper &operator=(const ControllerReaper &) = delete;

	/**
	 * Enqueue an arbitrary teardown job for the worker thread to run
	 * asynchronously (FIFO order relative to every other queued job, whether
	 * it arrived via this overload or the controller convenience overload
	 * below).  Safe to call from the Qt UI thread — this only ever takes a
	 * short lock, never blocks on the job itself.
	 *
	 * If called after shutdown() has already been requested (a stray
	 * caller racing the tail end of obs_module_unload), the job runs
	 * synchronously on the calling thread instead of being silently dropped
	 * — by that point in the plugin's lifecycle everything is already
	 * tearing down, so this is an acceptable, rare fallback.
	 */
	void enqueue(std::function<void()> job);

	/**
	 * Convenience overload, kept for the original teardown use: enqueue a
	 * controller for asynchronous shutdown_blocking() + release.  Equivalent
	 * to `enqueue([c = std::move(controller)] { c->shutdown_blocking(); })`.
	 */
	void enqueue(std::shared_ptr<OutputController> controller);

	/**
	 * Signal the worker to drain the remaining queue and exit, then
	 * block-join it.  Idempotent — safe to call more than once (or not at
	 * all; the destructor calls it as a safety net).  Must be called from
	 * obs_module_unload(), after all controllers have been enqueued.
	 */
	void shutdown();

private:
	void worker_loop();

	std::mutex                         m_mutex;
	std::condition_variable            m_cv;
	std::deque<std::function<void()>>  m_queue;
	bool                                m_shutting_down {false};
	std::thread                        m_worker;
};

} // namespace smulti
