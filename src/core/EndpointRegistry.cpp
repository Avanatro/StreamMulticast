/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "EndpointRegistry.hpp"
#include "../pipeline/OutputController.hpp"
#include "../pipeline/ControllerReaper.hpp"
#include "../plugin-support.h"

#include <algorithm>
#include <cassert>

namespace smulti {

/* -----------------------------------------------------------------------
 * Constructor / Destructor
 * ----------------------------------------------------------------------- */
EndpointRegistry::EndpointRegistry(ConfigStore &store, ControllerReaper &reaper)
	: m_store(store)
	, m_reaper(reaper)
{
	/* Populate from the already-loaded ConfigStore */
	auto loaded = store.endpoints();
	std::sort(loaded.begin(), loaded.end(),
	          [](const Endpoint &a, const Endpoint &b) {
		          return a.sort_order < b.sort_order;
	          });

	for (auto &ep : loaded) {
		m_controllers[ep.id] = std::make_shared<OutputController>(ep, m_reaper);
		m_endpoints.push_back(std::move(ep));
	}

	obs_log(LOG_INFO, "EndpointRegistry: initialised with %zu endpoints", m_endpoints.size());
}

EndpointRegistry::~EndpointRegistry()
{
	/* Hand every remaining controller off to the ControllerReaper instead of
	 * destroying it here — the reaper is drained and block-joined afterwards
	 * by obs_module_unload(), off whatever thread destroys this registry. */
	for (auto &pair : m_controllers)
		m_reaper.enqueue(std::move(pair.second));
	m_controllers.clear();
}

/* -----------------------------------------------------------------------
 * Query
 * ----------------------------------------------------------------------- */
std::vector<Endpoint> EndpointRegistry::all() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_endpoints;
}

const Endpoint *EndpointRegistry::find(const std::string &id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (const auto &ep : m_endpoints) {
		if (ep.id == id)
			return &ep;
	}
	return nullptr;
}

std::shared_ptr<OutputController> EndpointRegistry::controller_for(const std::string &id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	auto it = m_controllers.find(id);
	return it != m_controllers.end() ? it->second : nullptr;
}

size_t EndpointRegistry::count() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_endpoints.size();
}

/* -----------------------------------------------------------------------
 * add()
 * ----------------------------------------------------------------------- */
void EndpointRegistry::add(Endpoint ep)
{
	/* Assign sort order */
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		int max_order = 0;
		for (const auto &e : m_endpoints)
			max_order = std::max(max_order, e.sort_order);
		ep.sort_order = max_order + 1;

		m_controllers[ep.id] = std::make_shared<OutputController>(ep, m_reaper);
		m_endpoints.push_back(ep);
	}

	obs_log(LOG_INFO, "EndpointRegistry: added endpoint '%s' (id=%s)",
	        ep.name.c_str(), ep.id.c_str());

	notify_observers(ChangeKind::Added, ep);
	persist();
}

/* -----------------------------------------------------------------------
 * update()
 * ----------------------------------------------------------------------- */
void EndpointRegistry::update(const Endpoint &ep)
{
	bool was_running = false;
	std::shared_ptr<OutputController> old_ctrl;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = std::find_if(m_endpoints.begin(), m_endpoints.end(),
		                       [&](const Endpoint &e) { return e.id == ep.id; });
		if (it == m_endpoints.end()) {
			obs_log(LOG_WARNING, "EndpointRegistry::update — id not found: %s", ep.id.c_str());
			return;
		}

		*it = ep;

		/* Recreate controller with new settings.  The OLD controller is
		 * extracted here, WHILE HOLDING m_mutex; stop()/enqueue() happen
		 * AFTER the lock is released below (see item 6 of the fix-round 1
		 * decision log entry — stop() itself must never run under this
		 * lock, not just the reaper hand-off).  was_running gates only
		 * whether the NEW controller gets started afterward. */
		auto ctrl_it = m_controllers.find(ep.id);
		if (ctrl_it != m_controllers.end()) {
			was_running = ctrl_it->second->is_running();
			old_ctrl = std::move(ctrl_it->second);
			ctrl_it->second = std::make_shared<OutputController>(ep, m_reaper);
		}
	}

	if (old_ctrl) {
		/* stop() is cheap and non-blocking now (see OutputController's
		 * detach-and-reaper rule) — call it unconditionally rather than only
		 * when connection settings changed.  The old code's conn_changed
		 * gate left a window where a Live-but-unrelated-settings-changed
		 * controller kept streaming, un-stopped, while the just-created NEW
		 * controller was started right below — briefly double-streaming to
		 * the same endpoint until the reaper's shutdown_blocking() caught up
		 * with the old one asynchronously.  Always stopping first also
		 * closes that window for a Reconnecting old controller, which
		 * is_running() never covered. */
		old_ctrl->stop();
		m_reaper.enqueue(old_ctrl);
	}

	if (was_running) {
		/* Restart outside the lock */
		auto ctrl = controller_for(ep.id);
		if (ctrl)
			ctrl->start();
	}

	notify_observers(ChangeKind::Updated, ep);
	persist();
}

/* -----------------------------------------------------------------------
 * remove()
 * ----------------------------------------------------------------------- */
void EndpointRegistry::remove(const std::string &id)
{
	Endpoint removed_ep;
	std::shared_ptr<OutputController> old_ctrl;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = std::find_if(m_endpoints.begin(), m_endpoints.end(),
		                       [&](const Endpoint &e) { return e.id == id; });
		if (it == m_endpoints.end()) {
			obs_log(LOG_WARNING, "EndpointRegistry::remove — id not found: %s", id.c_str());
			return;
		}
		removed_ep = *it;

		/* Extract the controller WHILE HOLDING m_mutex; stop()/enqueue() it
		 * to the ControllerReaper AFTER releasing the lock (below) — stop()
		 * itself must never run under this lock either, not just the reaper
		 * hand-off (see item 6 of the fix-round 1 decision log entry). */
		auto ctrl_it = m_controllers.find(id);
		if (ctrl_it != m_controllers.end()) {
			old_ctrl = std::move(ctrl_it->second);
			m_controllers.erase(ctrl_it);
		}

		m_endpoints.erase(it);
	}

	if (old_ctrl) {
		/* stop() is cheap and non-blocking now — call it unconditionally
		 * (previously gated on is_running(), which excludes Reconnecting;
		 * that gap let a Reconnecting controller reach the reaper's
		 * shutdown_blocking() with its reconnect thread still live and no
		 * signal-disconnect done first). */
		old_ctrl->stop();
		m_reaper.enqueue(old_ctrl);
	}

	obs_log(LOG_INFO, "EndpointRegistry: removed endpoint '%s'", removed_ep.name.c_str());
	notify_observers(ChangeKind::Removed, removed_ep);
	persist();
}

/* -----------------------------------------------------------------------
 * reorder()
 * ----------------------------------------------------------------------- */
void EndpointRegistry::reorder(const std::vector<std::string> &ordered_ids)
{
	std::vector<Endpoint> updated;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (int i = 0; i < static_cast<int>(ordered_ids.size()); ++i) {
			auto it = std::find_if(m_endpoints.begin(), m_endpoints.end(),
			                       [&](const Endpoint &e) { return e.id == ordered_ids[i]; });
			if (it != m_endpoints.end()) {
				it->sort_order = i;
				updated.push_back(*it);
			}
		}
		/* Re-sort the internal list */
		std::sort(m_endpoints.begin(), m_endpoints.end(),
		          [](const Endpoint &a, const Endpoint &b) {
			          return a.sort_order < b.sort_order;
		          });
	}

	for (const auto &ep : updated)
		notify_observers(ChangeKind::Updated, ep);
	persist();
}

/* -----------------------------------------------------------------------
 * Observer management
 * ----------------------------------------------------------------------- */
int EndpointRegistry::register_observer(RegistryObserver obs)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	int token = m_next_observer_token++;
	m_observers.emplace_back(token, std::move(obs));
	return token;
}

void EndpointRegistry::unregister_observer(int token)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_observers.erase(
		std::remove_if(m_observers.begin(), m_observers.end(),
		               [token](const auto &pair) { return pair.first == token; }),
		m_observers.end());
}

/* -----------------------------------------------------------------------
 * notify_observers — always called WITHOUT the mutex held
 * ----------------------------------------------------------------------- */
void EndpointRegistry::notify_observers(ChangeKind kind, const Endpoint &ep)
{
	/* Snapshot observers list to avoid holding lock during callbacks */
	std::vector<RegistryObserver> snapshot;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		snapshot.reserve(m_observers.size());
		for (const auto &pair : m_observers)
			snapshot.push_back(pair.second);
	}

	for (const auto &obs : snapshot)
		obs(kind, ep);
}

/* -----------------------------------------------------------------------
 * persist — triggers debounced ConfigStore save
 * ----------------------------------------------------------------------- */
void EndpointRegistry::persist()
{
	/* Schedule a debounced save — cheap, non-blocking */
	std::lock_guard<std::mutex> lock(m_mutex);
	m_store.schedule_save(m_endpoints);
}

} // namespace smulti
