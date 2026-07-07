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
 * The dock is parented to OBS's QMainWindow ONLY on the success path of that
 * call — OBSStudioAPI::obs_frontend_add_dock_by_id() returns false BEFORE
 * calling setWidget() when the dock id is already registered, in which case
 * the caller (plugin-main.cpp) still owns this object and is responsible for
 * deleting it.  On success, Qt takes ownership and plugin-main.cpp must not
 * delete it — it explicitly unregisters it instead, via
 * obs_frontend_remove_dock(), in obs_module_unload().
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
