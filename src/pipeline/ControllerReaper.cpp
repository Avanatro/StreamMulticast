/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "ControllerReaper.hpp"
#include "OutputController.hpp"
#include "../plugin-support.h"

#include <exception>

namespace smulti {

ControllerReaper::ControllerReaper()
{
	m_worker = std::thread(&ControllerReaper::worker_loop, this);
}

ControllerReaper::~ControllerReaper()
{
	/* Safety net — the normal path is an explicit shutdown() call from
	 * obs_module_unload().  This guarantees we never leak a running (let
	 * alone detached) worker thread regardless of how this object is torn
	 * down. */
	shutdown();
}

void ControllerReaper::enqueue(std::function<void()> job)
{
	if (!job)
		return;

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_shutting_down) {
			m_queue.push_back(std::move(job));
			m_cv.notify_one();
			return;
		}
	}

	/* Past shutdown() — nobody will ever drain this queue again.  Run the
	 * job synchronously right here instead of silently dropping it.  This
	 * only happens for a stray caller racing the very tail of
	 * obs_module_unload(), at which point the whole plugin is already
	 * unwinding. */
	obs_log(LOG_WARNING, "ControllerReaper: enqueue() after shutdown() — "
	                      "running job synchronously on the caller's thread");
	job();
}

void ControllerReaper::enqueue(std::shared_ptr<OutputController> controller)
{
	if (!controller)
		return;

	enqueue([c = std::move(controller)]() { c->shutdown_blocking(); });
}

void ControllerReaper::shutdown()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_shutting_down)
			return; // idempotent
		m_shutting_down = true;
	}
	m_cv.notify_one();

	if (m_worker.joinable())
		m_worker.join(); // block-join — NEVER detach, see class doc comment
}

void ControllerReaper::worker_loop()
{
	for (;;) {
		std::function<void()> job;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait(lock, [this] { return !m_queue.empty() || m_shutting_down; });

			if (!m_queue.empty()) {
				job = std::move(m_queue.front());
				m_queue.pop_front();
			} else {
				break; // m_shutting_down and queue fully drained
			}
		}

		/* The job does the real (potentially long) work; this is exactly the
		 * point of running on this dedicated thread instead of the Qt UI
		 * thread.  For a controller-shutdown job, if this was the last
		 * reference, ~OutputController runs right after shutdown_blocking()
		 * returns, also on this thread — its own shutdown_blocking() call is
		 * then a no-op (idempotency guard).
		 *
		 * Wrapped in try/catch (fix-round 2, 2026-07-07): an exception escaping
		 * job() would otherwise propagate out of this thread function and call
		 * std::terminate() — crashing all of OBS.  Log and keep the worker
		 * running so one bad job never kills the reaper thread (which would
		 * strand every subsequent teardown, including obs_module_unload()'s
		 * final drain-and-join). */
		try {
			job();
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "ControllerReaper: teardown job threw an "
			                    "exception — %s (continuing)", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "ControllerReaper: teardown job threw a "
			                    "non-std exception (continuing)");
		}
	}
}

} // namespace smulti
