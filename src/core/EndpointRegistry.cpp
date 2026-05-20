/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "EndpointRegistry.hpp"
#include "../pipeline/OutputController.hpp"
#include "../plugin-support.h"

#include <algorithm>
#include <cassert>

namespace smulti {

/* -----------------------------------------------------------------------
 * Constructor / Destructor
 * ----------------------------------------------------------------------- */
EndpointRegistry::EndpointRegistry(ConfigStore &store)
	: m_store(store)
{
	/* Populate from the already-loaded ConfigStore */
	auto loaded = store.endpoints();
	std::sort(loaded.begin(), loaded.end(),
	          [](const Endpoint &a, const Endpoint &b) {
		          return a.sort_order < b.sort_order;
	          });

	for (auto &ep : loaded) {
		m_controllers[ep.id] = std::make_unique<OutputController>(ep);
		m_endpoints.push_back(std::move(ep));
	}

	obs_log(LOG_INFO, "EndpointRegistry: initialised with %zu endpoints", m_endpoints.size());
}

EndpointRegistry::~EndpointRegistry()
{
	/* Destroy controllers first — they stop their outputs */
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

OutputController *EndpointRegistry::controller_for(const std::string &id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	auto it = m_controllers.find(id);
	return it != m_controllers.end() ? it->second.get() : nullptr;
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

		m_controllers[ep.id] = std::make_unique<OutputController>(ep);
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
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = std::find_if(m_endpoints.begin(), m_endpoints.end(),
		                       [&](const Endpoint &e) { return e.id == ep.id; });
		if (it == m_endpoints.end()) {
			obs_log(LOG_WARNING, "EndpointRegistry::update — id not found: %s", ep.id.c_str());
			return;
		}

		const bool conn_changed = (it->server_url != ep.server_url ||
		                           it->stream_key != ep.stream_key ||
		                           it->encoder_backend != ep.encoder_backend ||
		                           it->video_bitrate_kbps != ep.video_bitrate_kbps ||
		                           it->audio_bitrate_kbps != ep.audio_bitrate_kbps);

		*it = ep;

		/* Restart controller if connection settings changed while running */
		auto ctrl_it = m_controllers.find(ep.id);
		if (ctrl_it != m_controllers.end()) {
			was_running = ctrl_it->second->is_running();
			if (conn_changed && was_running)
				ctrl_it->second->stop();
			/* Recreate controller with new settings */
			ctrl_it->second = std::make_unique<OutputController>(ep);
		}
	}

	if (was_running) {
		/* Restart outside the lock */
		auto *ctrl = controller_for(ep.id);
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
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = std::find_if(m_endpoints.begin(), m_endpoints.end(),
		                       [&](const Endpoint &e) { return e.id == id; });
		if (it == m_endpoints.end()) {
			obs_log(LOG_WARNING, "EndpointRegistry::remove — id not found: %s", id.c_str());
			return;
		}
		removed_ep = *it;

		/* Stop the output before destroying the controller */
		auto ctrl_it = m_controllers.find(id);
		if (ctrl_it != m_controllers.end()) {
			if (ctrl_it->second->is_running())
				ctrl_it->second->stop();
			m_controllers.erase(ctrl_it);
		}

		m_endpoints.erase(it);
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
