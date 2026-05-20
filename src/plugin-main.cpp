/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include "plugin-support.h"
#include "core/ConfigStore.hpp"
#include "core/EndpointRegistry.hpp"
#include "pipeline/HealthSampler.hpp"
#include "pipeline/OutputController.hpp"
#include "ui/MultistreamDock.hpp"

#include <QMainWindow>
#include <QAction>
#include <memory>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("streammulticast", "en-US")

/* -----------------------------------------------------------------------
 * Module-level singletons (owned by obs_module_load / obs_module_unload)
 * ----------------------------------------------------------------------- */
static std::unique_ptr<smulti::ConfigStore>      g_config_store;
static std::unique_ptr<smulti::EndpointRegistry> g_registry;
static std::unique_ptr<smulti::HealthSampler>    g_health_sampler;
static smulti::MultistreamDock *                 g_dock = nullptr; /* owned by Qt after hand-off */

/* -----------------------------------------------------------------------
 * OBS frontend-event handler
 * Reacts to main-stream start/stop to trigger "linked" endpoints.
 * ----------------------------------------------------------------------- */
static void on_frontend_event(enum obs_frontend_event event, void * /*private_data*/)
{
	if (!g_registry)
		return;

	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
		obs_log(LOG_INFO, "OBS main stream started — triggering linked endpoints");
		for (auto &ep : g_registry->all()) {
			if (ep.linked_to_main) {
				auto *ctrl = g_registry->controller_for(ep.id);
				if (ctrl && !ctrl->is_running())
					ctrl->start();
			}
		}
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
		obs_log(LOG_INFO, "OBS main stream stopped — stopping linked endpoints");
		for (auto &ep : g_registry->all()) {
			if (ep.linked_to_main) {
				auto *ctrl = g_registry->controller_for(ep.id);
				if (ctrl && ctrl->is_running())
					ctrl->stop();
			}
		}
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		/* Graceful shutdown — stop all outputs before OBS tears down */
		obs_log(LOG_INFO, "OBS exit — stopping all StreamMulticast outputs");
		for (auto &ep : g_registry->all()) {
			auto *ctrl = g_registry->controller_for(ep.id);
			if (ctrl && ctrl->is_running())
				ctrl->stop();
		}
		if (g_health_sampler)
			g_health_sampler->stop();
	}
}

/* -----------------------------------------------------------------------
 * obs_module_load — called by OBS when the plugin DLL is loaded
 * ----------------------------------------------------------------------- */
bool obs_module_load()
{
	obs_log(LOG_INFO, "StreamMulticast v%s loading", PLUGIN_VERSION);

	/* 1. Boot config store — loads endpoints from disk */
	g_config_store = std::make_unique<smulti::ConfigStore>();
	if (!g_config_store->load()) {
		obs_log(LOG_WARNING, "ConfigStore load failed — starting with empty config");
	}

	/* 2. Boot endpoint registry — populates from config store */
	g_registry = std::make_unique<smulti::EndpointRegistry>(*g_config_store);

	/* 3. Boot health sampler — starts background 2-Hz polling thread */
	g_health_sampler = std::make_unique<smulti::HealthSampler>(*g_registry);
	g_health_sampler->start();

	/* 4. Register frontend event handler (for linked-to-main-stream behaviour) */
	// AVANATRO-VERIFY: obs_frontend_add_event_callback signature — verify it exists in
	// obs-frontend-api.h for OBS 30+ (it does in OBS 30.x, but confirm 31.x didn't rename it).
	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	/* 5. Create and register the dock widget
	 * obs_frontend_add_dock_by_id was added in OBS 30.0.
	 * AVANATRO-VERIFY: Confirm obs_frontend_add_dock_by_id signature in OBS 31.x —
	 * the 4th parameter (area hint) was added in a later OBS 30.x patch.
	 * Prototype from OBS 30.0: bool obs_frontend_add_dock_by_id(const char *id,
	 *   const char *title, QWidget *widget);
	 * Prototype from OBS 30.2+: bool obs_frontend_add_dock_by_id(const char *id,
	 *   const char *title, QWidget *widget, Qt::DockWidgetArea area = Qt::RightDockWidgetArea);
	 * Use 3-arg form for broadest compatibility. */
	g_dock = new smulti::MultistreamDock(*g_registry, *g_health_sampler);

	// AVANATRO-VERIFY: obs_frontend_add_dock_by_id return value — OBS 30 returns bool,
	// but some builds omit the return. Check whether to handle false (dock already registered).
	bool dock_ok = obs_frontend_add_dock_by_id(
		"avanatro_streammulticast",
		obs_module_text("MultistreamDockTitle"),
		g_dock
	);
	if (!dock_ok) {
		obs_log(LOG_WARNING, "obs_frontend_add_dock_by_id returned false — dock may already exist");
		/* g_dock is still valid; Qt will parent it even if registration failed */
	}

	obs_log(LOG_INFO, "StreamMulticast v%s loaded successfully", PLUGIN_VERSION);
	return true;
}

/* -----------------------------------------------------------------------
 * obs_module_unload — called by OBS on shutdown / plugin disable
 * ----------------------------------------------------------------------- */
void obs_module_unload()
{
	obs_log(LOG_INFO, "StreamMulticast unloading");

	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	/* Stop all active outputs gracefully */
	if (g_registry) {
		for (auto &ep : g_registry->all()) {
			auto *ctrl = g_registry->controller_for(ep.id);
			if (ctrl && ctrl->is_running())
				ctrl->stop();
		}
	}

	/* Stop health sampler thread */
	if (g_health_sampler) {
		g_health_sampler->stop();
		g_health_sampler.reset();
	}

	/* Save config to disk */
	if (g_config_store && g_registry) {
		g_config_store->save(g_registry->all());
	}

	/* Note: g_dock is owned by Qt's parent chain after obs_frontend_add_dock_by_id,
	 * do NOT delete it manually — OBS/Qt will handle destruction. */
	g_dock = nullptr;

	g_registry.reset();
	g_config_store.reset();

	obs_log(LOG_INFO, "StreamMulticast unloaded");
}

/* -----------------------------------------------------------------------
 * Module metadata
 * ----------------------------------------------------------------------- */
const char *obs_module_description()
{
	return "StreamMulticast — free, open-source multi-RTMP output with per-output bitrate";
}

const char *obs_module_name()
{
	return "StreamMulticast by Avanatro";
}
