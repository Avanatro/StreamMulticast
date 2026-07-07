/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "Endpoint.hpp"
#include "ConfigStore.hpp"

#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <memory>
#include <unordered_map>

namespace smulti {

// Forward declare to avoid circular include
class OutputController;
class ControllerReaper;

/**
 * ChangeKind — what happened to an endpoint in the registry.
 */
enum class ChangeKind { Added, Removed, Updated };

/**
 * Observer callback type.
 * Called on the Qt UI thread via QMetaObject::invokeMethod when the registry
 * changes.  Observers must not hold the registry mutex when called.
 */
using RegistryObserver = std::function<void(ChangeKind, const Endpoint &)>;

/**
 * EndpointRegistry — in-memory list of all configured endpoints.
 *
 * Single source of truth for the endpoint list at runtime.
 * Owns OutputController instances (one per endpoint, created lazily on add),
 * held as shared_ptr so HealthSampler can keep sampling a controller that is
 * concurrently being replaced/removed here without racing its destruction —
 * see controller_for().  Notifies registered UI observers on every change.
 *
 * Thread-safety: all mutating methods are guarded by an internal mutex.
 * Observer callbacks are invoked via Qt's queued connection to guarantee
 * they run on the UI thread.
 *
 * Controller teardown: remove()/update() extract the OLD OutputController
 * shared_ptr from the map WHILE HOLDING m_mutex, release the lock, then hand
 * it to the ControllerReaper instead of letting it destruct synchronously
 * here.  ~OutputController (via shutdown_blocking()) can block indefinitely
 * on obs_output_release() if the RTMP send thread is stuck — doing that
 * while holding m_mutex would freeze every other endpoint's UI interaction
 * too.  See ControllerReaper for the full rationale.
 *
 * Lifecycle: created in obs_module_load(), destroyed in obs_module_unload().
 * The destructor enqueues all remaining controllers to the reaper rather
 * than destroying them directly — obs_module_unload() drains and
 * block-joins the reaper afterwards.
 */
class EndpointRegistry {
public:
	EndpointRegistry(ConfigStore &store, ControllerReaper &reaper);
	~EndpointRegistry();

	/* Non-copyable */
	EndpointRegistry(const EndpointRegistry &) = delete;
	EndpointRegistry &operator=(const EndpointRegistry &) = delete;

	/* ---- Query ---- */

	/** All endpoints, sorted by sort_order */
	std::vector<Endpoint> all() const;

	/** Find by UUID, returns nullptr if not found */
	const Endpoint *find(const std::string &id) const;

	/**
	 * OutputController for the given endpoint UUID (nullptr if not found).
	 * Returns a shared_ptr copy (not a raw pointer) so a caller — chiefly
	 * HealthSampler's background poll thread — can keep using the returned
	 * controller safely even if remove()/update() concurrently swaps or
	 * erases the registry's own entry for this id.
	 */
	std::shared_ptr<OutputController> controller_for(const std::string &id);

	/** Number of registered endpoints */
	size_t count() const;

	/* ---- Mutation ---- */

	/**
	 * Add a new endpoint.  Assigns sort_order = max_existing + 1.
	 * Creates an OutputController for it.
	 * Notifies observers with ChangeKind::Added.
	 * Triggers a debounced ConfigStore save.
	 */
	void add(Endpoint ep);

	/**
	 * Update an existing endpoint by UUID.  If the endpoint was running and
	 * connection settings changed, the OutputController is restarted.
	 * Notifies observers with ChangeKind::Updated.
	 * Triggers a debounced ConfigStore save.
	 */
	void update(const Endpoint &ep);

	/**
	 * Remove an endpoint by UUID.  Stops its OutputController first.
	 * Notifies observers with ChangeKind::Removed.
	 * Triggers a debounced ConfigStore save.
	 */
	void remove(const std::string &id);

	/**
	 * Reorder — replace the full list (after drag-to-reorder in ConfigTab).
	 * Updates sort_order fields; notifies all observers with Updated.
	 */
	void reorder(const std::vector<std::string> &ordered_ids);

	/* ---- Observers ---- */

	/**
	 * Register a change observer.  Returns an opaque token that the caller
	 * must pass to unregister() to prevent dangling callbacks.
	 */
	int register_observer(RegistryObserver obs);

	/** Unregister by token from register_observer() */
	void unregister_observer(int token);

private:
	void notify_observers(ChangeKind kind, const Endpoint &ep);
	void persist();

	mutable std::mutex m_mutex;

	std::vector<Endpoint>                                    m_endpoints;
	std::unordered_map<std::string, std::shared_ptr<OutputController>> m_controllers;

	std::vector<std::pair<int, RegistryObserver>> m_observers;
	int m_next_observer_token = 1;

	ConfigStore      &m_store;
	ControllerReaper &m_reaper;
};

} // namespace smulti
