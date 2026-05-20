/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "Endpoint.hpp"

#include <vector>
#include <string>
#include <functional>
#include <chrono>
#include <mutex>
#include <atomic>
#include <thread>

namespace smulti {

/**
 * ConfigStore — persistent storage for all Endpoint configurations.
 *
 * Persists to:
 *   %APPDATA%/obs-studio/plugin_config/streammulticast/config.json
 * via obs_module_get_config_path().
 *
 * Auto-saves with 500 ms debounce (schedule_save() starts the debounce timer;
 * a background thread fires the actual write).  Thread-safe for concurrent
 * schedule_save() calls from the Qt UI thread.
 *
 * Schema version is embedded in the JSON root so future migrations can detect
 * old formats.
 *
 * Current schema version: 1
 */
class ConfigStore {
public:
	ConfigStore();
	~ConfigStore();

	/* Non-copyable, non-movable (owns a mutex and background thread) */
	ConfigStore(const ConfigStore &) = delete;
	ConfigStore &operator=(const ConfigStore &) = delete;

	/**
	 * Load configuration from disk.
	 * Returns true on success, false if the file doesn't exist (first run)
	 * or is corrupt (resets to empty).  Always safe to call from main thread
	 * before the save thread is started.
	 */
	bool load();

	/**
	 * Save the given endpoint list to disk immediately (synchronous).
	 * Called from obs_module_unload to guarantee flush on exit.
	 */
	bool save(const std::vector<Endpoint> &endpoints);

	/**
	 * Schedule a debounced save (500 ms delay).  Cheap to call on every UI
	 * change — the actual disk write happens at most once per 500 ms.
	 *
	 * AVANATRO-VERIFY: obs_module_get_config_path ownership — the returned
	 * char* must be freed with bfree().  Confirm this is still the case in
	 * OBS 31.x (was true in OBS 30.x).
	 */
	void schedule_save(const std::vector<Endpoint> &endpoints);

	/** Loaded endpoints (copy) — call after load() succeeds */
	std::vector<Endpoint> endpoints() const;

	/** Current on-disk config path (may be empty before load()) */
	std::string config_path() const { return m_config_path; }

private:
	/** Resolve (and create) the plugin config directory, returning the full path */
	static std::string resolve_config_path();

	/** Low-level write — serialises endpoint list to JSON and writes to path */
	bool write_to_disk(const std::vector<Endpoint> &endpoints, const std::string &path);

	/** Low-level read — deserialises JSON from path into endpoint list */
	bool read_from_disk(const std::string &path, std::vector<Endpoint> &out);

	/** Migrate schema from older versions */
	void migrate(int from_version, std::vector<Endpoint> &endpoints);

	mutable std::mutex       m_mutex;
	std::string              m_config_path;
	std::vector<Endpoint>    m_endpoints;

	/* Debounce save */
	std::atomic<bool>        m_save_pending{false};
	std::vector<Endpoint>    m_pending_endpoints;
	std::mutex               m_pending_mutex;
	std::thread              m_save_thread;
	std::atomic<bool>        m_save_thread_running{false};

	void save_thread_func();
};

} // namespace smulti
