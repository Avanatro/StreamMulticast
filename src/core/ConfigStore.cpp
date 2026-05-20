/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "ConfigStore.hpp"
#include "../plugin-support.h"

#include <obs-module.h>
#include <obs-data.h>
#include <util/platform.h>
#include <util/base.h>

#include <chrono>
#include <thread>
#include <filesystem>

namespace smulti {

static constexpr int CURRENT_SCHEMA_VERSION = 1;
static constexpr int SAVE_DEBOUNCE_MS       = 500;

/* -----------------------------------------------------------------------
 * Constructor / Destructor
 * ----------------------------------------------------------------------- */
ConfigStore::ConfigStore()
{
	m_config_path = resolve_config_path();
	m_save_thread_running.store(true);
	m_save_thread = std::thread(&ConfigStore::save_thread_func, this);
}

ConfigStore::~ConfigStore()
{
	m_save_thread_running.store(false);
	if (m_save_thread.joinable())
		m_save_thread.join();
}

/* -----------------------------------------------------------------------
 * Path resolution
 *
 * AVANATRO-VERIFY: obs_module_get_config_path — the returned char* must
 * be freed with bfree().  Confirm API in OBS 31.x.
 * Also verify that the path is created automatically or if we need
 * os_mkdirs().
 * ----------------------------------------------------------------------- */
std::string ConfigStore::resolve_config_path()
{
	// obs_module_get_config_path allocates — caller must bfree()
	char *raw = obs_module_get_config_path(obs_current_module(), "config.json");
	if (!raw) {
		obs_log(LOG_WARNING, "obs_module_get_config_path returned null — using fallback path");
		return "";
	}
	std::string path(raw);
	bfree(raw);

	/* Ensure directory exists */
	std::filesystem::path dir = std::filesystem::path(path).parent_path();
	if (!std::filesystem::exists(dir)) {
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		if (ec) {
			obs_log(LOG_WARNING, "Could not create config dir: %s",
			        ec.message().c_str());
		}
	}

	return path;
}

/* -----------------------------------------------------------------------
 * load()
 * ----------------------------------------------------------------------- */
bool ConfigStore::load()
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_config_path.empty()) {
		obs_log(LOG_WARNING, "ConfigStore: no config path — cannot load");
		return false;
	}

	std::vector<Endpoint> loaded;
	if (!read_from_disk(m_config_path, loaded)) {
		obs_log(LOG_INFO, "ConfigStore: config.json not found or corrupt — starting fresh");
		m_endpoints.clear();
		return false;
	}

	m_endpoints = std::move(loaded);
	obs_log(LOG_INFO, "ConfigStore: loaded %zu endpoints from %s",
	        m_endpoints.size(), m_config_path.c_str());
	return true;
}

/* -----------------------------------------------------------------------
 * save() — synchronous (used from obs_module_unload)
 * ----------------------------------------------------------------------- */
bool ConfigStore::save(const std::vector<Endpoint> &endpoints)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_endpoints = endpoints;

	if (m_config_path.empty()) {
		obs_log(LOG_WARNING, "ConfigStore: no config path — cannot save");
		return false;
	}

	return write_to_disk(endpoints, m_config_path);
}

/* -----------------------------------------------------------------------
 * schedule_save() — debounced (used from UI on every change)
 * ----------------------------------------------------------------------- */
void ConfigStore::schedule_save(const std::vector<Endpoint> &endpoints)
{
	{
		std::lock_guard<std::mutex> lock(m_pending_mutex);
		m_pending_endpoints = endpoints;
		m_save_pending.store(true);
	}
	/* The save_thread_func() will pick this up within SAVE_DEBOUNCE_MS */
}

/* -----------------------------------------------------------------------
 * save_thread_func — background thread, wakes every 100 ms
 * ----------------------------------------------------------------------- */
void ConfigStore::save_thread_func()
{
	using namespace std::chrono;
	time_point<steady_clock> last_change = steady_clock::now();

	while (m_save_thread_running.load()) {
		std::this_thread::sleep_for(milliseconds(100));

		if (!m_save_pending.load())
			continue;

		auto now = steady_clock::now();
		if (duration_cast<milliseconds>(now - last_change).count() < SAVE_DEBOUNCE_MS) {
			last_change = now;
			continue;
		}

		/* Debounce elapsed — perform the save */
		std::vector<Endpoint> to_save;
		{
			std::lock_guard<std::mutex> lock(m_pending_mutex);
			to_save = m_pending_endpoints;
			m_save_pending.store(false);
		}

		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_endpoints = to_save;
			if (!m_config_path.empty())
				write_to_disk(to_save, m_config_path);
		}

		last_change = steady_clock::now();
	}

	/* Final flush if something was pending when we shut down */
	if (m_save_pending.load()) {
		std::lock_guard<std::mutex> pending_lock(m_pending_mutex);
		std::lock_guard<std::mutex> main_lock(m_mutex);
		if (!m_config_path.empty())
			write_to_disk(m_pending_endpoints, m_config_path);
	}
}

/* -----------------------------------------------------------------------
 * endpoints() — returns a copy (thread-safe)
 * ----------------------------------------------------------------------- */
std::vector<Endpoint> ConfigStore::endpoints() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_endpoints;
}

/* -----------------------------------------------------------------------
 * write_to_disk — serialise endpoint list to JSON
 *
 * AVANATRO-VERIFY: obs_data_save_json — confirm function name in OBS 31.x
 * (was obs_data_save_json() in OBS 29-30, may be obs_data_save_json_safe()
 * in newer builds).  Use obs_data_save_json_safe with tmp/bak files.
 * ----------------------------------------------------------------------- */
bool ConfigStore::write_to_disk(const std::vector<Endpoint> &endpoints, const std::string &path)
{
	obs_data_t *root = obs_data_create();
	obs_data_set_int(root, "schema_version", CURRENT_SCHEMA_VERSION);

	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &ep : endpoints) {
		obs_data_t *ep_data = ep.serialize();
		obs_data_array_push_back(arr, ep_data);
		obs_data_release(ep_data); // array addref'd — safe to release our handle
	}
	obs_data_set_array(root, "endpoints", arr);
	obs_data_array_release(arr);

	// AVANATRO-VERIFY: obs_data_save_json_safe signature and availability in OBS 31.x.
	// The safe variant writes to .tmp then renames, preventing corruption on crash.
	bool ok = obs_data_save_json_safe(root, path.c_str(), "tmp", "bak");
	if (!ok)
		obs_log(LOG_WARNING, "ConfigStore: obs_data_save_json_safe failed for %s", path.c_str());

	obs_data_release(root);
	return ok;
}

/* -----------------------------------------------------------------------
 * read_from_disk — deserialise endpoint list from JSON
 * ----------------------------------------------------------------------- */
bool ConfigStore::read_from_disk(const std::string &path, std::vector<Endpoint> &out)
{
	if (path.empty() || !std::filesystem::exists(path))
		return false;

	// AVANATRO-VERIFY: obs_data_create_from_json_file — confirm it returns nullptr on
	// missing file vs. empty/corrupt file, and whether it logs internally.
	obs_data_t *root = obs_data_create_from_json_file(path.c_str());
	if (!root)
		return false;

	int schema_version = static_cast<int>(obs_data_get_int(root, "schema_version"));

	obs_data_array_t *arr = obs_data_get_array(root, "endpoints");
	if (!arr) {
		obs_data_release(root);
		return false;
	}

	size_t count = obs_data_array_count(arr);
	out.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *ep_data = obs_data_array_item(arr, i);
		Endpoint ep = Endpoint::deserialize(ep_data);
		obs_data_release(ep_data);
		if (!ep.id.empty())
			out.push_back(std::move(ep));
	}

	obs_data_array_release(arr);

	if (schema_version < CURRENT_SCHEMA_VERSION)
		migrate(schema_version, out);

	obs_data_release(root);
	return true;
}

/* -----------------------------------------------------------------------
 * migrate — for future schema changes
 * ----------------------------------------------------------------------- */
void ConfigStore::migrate(int from_version, std::vector<Endpoint> &/*endpoints*/)
{
	obs_log(LOG_INFO, "ConfigStore: migrating config from schema v%d to v%d",
	        from_version, CURRENT_SCHEMA_VERSION);
	/* No migrations needed from v1 → v1 */
}

} // namespace smulti
