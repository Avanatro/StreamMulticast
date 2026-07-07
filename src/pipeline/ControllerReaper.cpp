/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "ControllerReaper.hpp"
#include "OutputController.hpp"
#include "../plugin-support.h"

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

void ControllerReaper::enqueue(std::shared_ptr<OutputController> controller)
{
	if (!controller)
		return;

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_shutting_down) {
			m_queue.push_back(std::move(controller));
			m_cv.notify_one();
			return;
		}
	}

	/* Past shutdown() — nobody will ever drain this queue again.  Tear down
	 * synchronously right here instead of silently dropping the controller.
	 * This only happens for a stray remove()/update() racing the very tail
	 * of obs_module_unload(), at which point the whole plugin is already
	 * unwinding. */
	obs_log(LOG_WARNING, "ControllerReaper: enqueue() after shutdown() — "
	                      "tearing down synchronously on the caller's thread");
	controller->shutdown_blocking();
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
		std::shared_ptr<OutputController> controller;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait(lock, [this] { return !m_queue.empty() || m_shutting_down; });

			if (!m_queue.empty()) {
				controller = std::move(m_queue.front());
				m_queue.pop_front();
			} else {
				break; // m_shutting_down and queue fully drained
			}
		}

		/* shutdown_blocking() does the real (potentially long) work; this is
		 * exactly the point of running on this dedicated thread instead of
		 * the Qt UI thread.  If this was the last reference, ~OutputController
		 * runs right after, also on this thread — its own shutdown_blocking()
		 * call is then a no-op (idempotency guard). */
		controller->shutdown_blocking();
	}
}

} // namespace smulti
