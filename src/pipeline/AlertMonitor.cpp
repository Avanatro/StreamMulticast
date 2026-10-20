/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

#include "AlertMonitor.hpp"

#include "../core/EndpointRegistry.hpp"
#include "../plugin-support.h"
#include "HealthSampler.hpp"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

/* Vendored obs-websocket vendor API, unmodified: obsproject/obs-websocket lib/obs-websocket-api.h at commit
 * 68bebc28be57a8ca371404182113e16eeac74cbf (see src/third_party/obs-websocket/README.md). It uses assert()
 * and strlen() without including their headers. */
#include <cassert>
#include <cstring>
#include "../third_party/obs-websocket/obs-websocket-api.h"

#include <chrono>
#include <string>
#include <vector>

namespace smulti {

namespace {

/* Set once in obs_module_post_load(); nullptr when obs-websocket is unavailable. */
obs_websocket_vendor g_vendor = nullptr;

constexpr const char *kVendorName = "streammulticast";
constexpr const char *kEventName  = "alert";
constexpr int         kTickMs     = 500;

int64_t steady_now_ms()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string settings_path()
{
	char *raw = obs_module_get_config_path(obs_current_module(), kAlertSettingsFile);
	std::string path = raw ? raw : "";
	bfree(raw);
	return path;
}

} // namespace

bool alerts_register_vendor()
{
	g_vendor = obs_websocket_register_vendor(kVendorName);
	if (!g_vendor) {
		obs_log(LOG_INFO, "obs-websocket vendor API not available: alerts work without the websocket event");
		return false;
	}
	obs_log(LOG_INFO, "obs-websocket vendor '%s' registered (event '%s')", kVendorName, kEventName);
	return true;
}

void alerts_release_vendor()
{
	g_vendor = nullptr;
}

AlertMonitor::AlertMonitor(EndpointRegistry &registry, HealthSampler &sampler, QObject *parent)
	: QObject(parent), m_registry(registry), m_sampler(sampler), m_timer(this)
{
	const std::string path = settings_path();
	obs_data_t *data = path.empty() ? nullptr : obs_data_create_from_json_file(path.c_str());
	m_settings = alert_settings_load(data);
	obs_data_release(data);

	m_timer.setInterval(kTickMs);
	connect(&m_timer, &QTimer::timeout, this, [this] { on_tick(); });
}

AlertMonitor::~AlertMonitor()
{
	stop();
}

void AlertMonitor::start()
{
	if (!m_timer.isActive())
		m_timer.start();
}

void AlertMonitor::stop()
{
	m_timer.stop();
}

void AlertMonitor::set_settings(const AlertSettings &settings)
{
	m_settings = settings;
	save_settings_file();
}

void AlertMonitor::save_settings_file() const
{
	const std::string path = settings_path();
	if (path.empty())
		return;
	const size_t slash = path.find_last_of('/');
	if (slash != std::string::npos)
		os_mkdirs(path.substr(0, slash).c_str());
	obs_data_t *data = obs_data_create();
	alert_settings_save(data, m_settings);
	if (!obs_data_save_json_safe(data, path.c_str(), "tmp", "bak"))
		obs_log(LOG_WARNING, "alerts: could not write %s", path.c_str());
	obs_data_release(data);
}

void AlertMonitor::on_tick()
{
	const int64_t now = steady_now_ms();
	const auto snapshots = m_sampler.snapshot();

	std::vector<AlertTransition> transitions;
	std::vector<std::string> ids;
	for (const auto &ep : m_registry.all()) {
		ids.push_back(ep.id);
		const auto it = snapshots.find(ep.id);
		if (it == snapshots.end())
			continue;
		const HealthSnapshot &snap = it->second;
		AlertInput in;
		in.endpoint_id   = ep.id;
		in.endpoint_name = ep.name;
		in.state         = snap.state;
		in.target_kbps   = snap.target_bitrate;
		in.actual_kbps   = snap.actual_bitrate;
		in.dropped_total = snap.dropped_frames;
		in.frames_total  = snap.total_frames;
		in.now_ms        = now;
		auto out = m_policy.update(in);
		transitions.insert(transitions.end(), out.begin(), out.end());
	}
	/* Removed endpoints: their alerts clear (and are reported once). */
	auto gone = m_policy.retain_only(ids);
	transitions.insert(transitions.end(), gone.begin(), gone.end());

	for (const auto &t : transitions)
		handle(t, now);
	if (!transitions.empty())
		emit levels_changed();
}

void AlertMonitor::handle(const AlertTransition &t, int64_t now_ms)
{
	obs_log(LOG_INFO, "alert %s: endpoint '%s' level=%s reason=%s state=%s", t.raised ? "raised" : "cleared",
		t.endpoint_name.c_str(), alert_level_name(t.level), t.reason.c_str(), alert_state_name(t.state));

#ifdef _WIN32
	if (t.raised && m_settings.sound_enabled && alert_sound_allowed(now_ms, m_last_sound_ms))
		PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
#else
	(void)now_ms;
#endif

	if (m_settings.websocket_enabled && g_vendor) {
		obs_data_t *payload = alert_event_payload(t);
		/* Does not take ownership of the payload. */
		obs_websocket_vendor_emit_event(g_vendor, kEventName, payload);
		obs_data_release(payload);
	}
}

} // namespace smulti
