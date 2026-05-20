/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <QtWidgets/QWidget>
#include <QtWidgets/QTabWidget>
#include <memory>

namespace smulti {

class EndpointRegistry;
class HealthSampler;
class HealthTab;
class ConfigTab;

/**
 * MultistreamDock — top-level Qt widget registered as an OBS dock.
 *
 * Hosts a QTabWidget with two tabs:
 *   Tab 0 — "Health"    (HealthTab — Live-Grid)
 *   Tab 1 — "Configure" (ConfigTab — Endpoint-Karten)
 *
 * Registered via obs_frontend_add_dock_by_id("avanatro_streammulticast", ...).
 * The dock is parented to OBS's QMainWindow after registration.
 *
 * AVANATRO-VERIFY: obs_frontend_add_dock_by_id — after calling this, Qt takes
 * ownership of the widget.  Do NOT delete MultistreamDock manually;
 * OBS/Qt will do it on shutdown.  Confirm this in OBS 31.x source.
 */
class MultistreamDock : public QWidget {
	Q_OBJECT

public:
	explicit MultistreamDock(EndpointRegistry &registry,
	                         HealthSampler    &sampler,
	                         QWidget          *parent = nullptr);
	~MultistreamDock() override = default;

private:
	void setup_ui();

	EndpointRegistry &m_registry;
	HealthSampler    &m_sampler;

	QTabWidget *m_tabs      {nullptr};
	HealthTab  *m_health_tab{nullptr};
	ConfigTab  *m_config_tab{nullptr};
};

} // namespace smulti
