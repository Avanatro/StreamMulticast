/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/AlertPolicy.hpp"

#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <cstdint>
#include <map>
#include <string>

namespace smulti {

class EndpointRegistry;
class HealthSampler;

/**
 * Registers the obs-websocket vendor "streammulticast". Call only from obs_module_post_load(): the
 * vendor API requires it. Returns false (and logs once at LOG_INFO) when obs-websocket is missing,
 * disabled or too old; alerts then work without the websocket event.
 */
bool alerts_register_vendor();

/** Forget the vendor handle (module unload). Later alert events are not sent. */
void alerts_release_vendor();

/**
 * AlertMonitor — UI-thread adapter around the pure AlertPolicy.
 *
 * Every 500 ms it reads HealthSampler::snapshot() and the endpoint names from EndpointRegistry, feeds
 * AlertPolicy, and for each edge it:
 *   - raise, sound enabled: plays the "SystemExclamation" alias (Win32 PlaySoundW, local only, no file
 *     shipped), at most once per 30 s across endpoints;
 *   - raise or clear, websocket enabled and vendor registered: emits vendor event "alert" with the
 *     payload from alert_event_payload() (no URL, key, token or last_error text);
 *   - emits levels_changed() so the UI can re-read levels().
 *
 * Threading: construct, start(), stop(), set_settings() and the getters on the UI thread only.
 * stop() must run before HealthSampler::stop().
 */
class AlertMonitor : public QObject {
	Q_OBJECT

public:
	AlertMonitor(EndpointRegistry &registry, HealthSampler &sampler, QObject *parent = nullptr);
	~AlertMonitor() override;

	void start();
	void stop();

	AlertSettings settings() const { return m_settings; }
	/** Applies and writes plugin_config/streammulticast/alerts.json. */
	void set_settings(const AlertSettings &settings);

	/** Highest level over all endpoints. */
	AlertLevel max_level() const { return m_policy.max_level(); }
	/** Endpoints that currently have an active alert (id -> level + reasons). */
	std::map<std::string, AlertStatus> statuses() const { return m_policy.statuses(); }

signals:
	/** An alert was raised or cleared. */
	void levels_changed();

private:
	void on_tick();
	void handle(const AlertTransition &transition, int64_t now_ms);
	void save_settings_file() const;

	EndpointRegistry &m_registry;
	HealthSampler    &m_sampler;
	AlertPolicy       m_policy;
	AlertSettings     m_settings;
	QTimer            m_timer;
	int64_t           m_last_sound_ms = -1;
};

} // namespace smulti
