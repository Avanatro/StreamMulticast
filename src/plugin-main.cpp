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
#include "pipeline/ControllerReaper.hpp"
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
static std::unique_ptr<smulti::ControllerReaper>  g_reaper;
static std::unique_ptr<smulti::ConfigStore>      g_config_store;
static std::unique_ptr<smulti::EndpointRegistry> g_registry;
static std::unique_ptr<smulti::HealthSampler>    g_health_sampler;
static smulti::MultistreamDock *                 g_dock = nullptr; /* owned by Qt only if registration succeeded — see obs_module_load */

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
				auto ctrl = g_registry->controller_for(ep.id);
				if (ctrl && !ctrl->is_running())
					ctrl->start();
			}
		}
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
		obs_log(LOG_INFO, "OBS main stream stopped — stopping linked endpoints");
		for (auto &ep : g_registry->all()) {
			if (ep.linked_to_main) {
				auto ctrl = g_registry->controller_for(ep.id);
				if (ctrl && ctrl->is_running())
					ctrl->stop();
			}
		}
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		/* Graceful shutdown — stop all outputs before OBS tears down */
		obs_log(LOG_INFO, "OBS exit — stopping all StreamMulticast outputs");
		for (auto &ep : g_registry->all()) {
			auto ctrl = g_registry->controller_for(ep.id);
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

	/* 1. Boot the controller reaper first — EndpointRegistry needs it for
	 * every controller it creates. */
	g_reaper = std::make_unique<smulti::ControllerReaper>();

	/* 2. Boot config store — loads endpoints from disk */
	g_config_store = std::make_unique<smulti::ConfigStore>();
	if (!g_config_store->load()) {
		obs_log(LOG_WARNING, "ConfigStore load failed — starting with empty config");
	}

	/* 3. Boot endpoint registry — populates from config store */
	g_registry = std::make_unique<smulti::EndpointRegistry>(*g_config_store, *g_reaper);

	/* 4. Boot health sampler — starts background 2-Hz polling thread */
	g_health_sampler = std::make_unique<smulti::HealthSampler>(*g_registry);
	g_health_sampler->start();

	/* 5. Register frontend event handler (for linked-to-main-stream behaviour) */
	// AVANATRO-VERIFY: obs_frontend_add_event_callback signature — verify it exists in
	// obs-frontend-api.h for OBS 30+ (it does in OBS 30.x, but confirm 31.x didn't rename it).
	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	/* 6. Create and register the dock widget
	 * obs_frontend_add_dock_by_id was added in OBS 30.0.
	 * AVANATRO-VERIFY: Confirm obs_frontend_add_dock_by_id signature in OBS 31.x —
	 * the 4th parameter (area hint) was added in a later OBS 30.x patch.
	 * Prototype from OBS 30.0: bool obs_frontend_add_dock_by_id(const char *id,
	 *   const char *title, QWidget *widget);
	 * Prototype from OBS 30.2+: bool obs_frontend_add_dock_by_id(const char *id,
	 *   const char *title, QWidget *widget, Qt::DockWidgetArea area = Qt::RightDockWidgetArea);
	 * Use 3-arg form for broadest compatibility. */
	g_dock = new smulti::MultistreamDock(*g_registry, *g_health_sampler);

	/* obs_frontend_add_dock_by_id() returns false BEFORE calling setWidget()
	 * on a duplicate dock id (see OBSStudioAPI.cpp: the id-collision check
	 * returns early, well before the dock->setWidget(widget) call) — so on
	 * failure g_dock has NOT been parented into any Qt object tree and we
	 * still own it exclusively.  It must be deleted here, or it leaks for
	 * the lifetime of the OBS process. */
	bool dock_ok = obs_frontend_add_dock_by_id(
		"avanatro_streammulticast",
		obs_module_text("MultistreamDockTitle"),
		g_dock
	);
	if (!dock_ok) {
		obs_log(LOG_WARNING, "obs_frontend_add_dock_by_id returned false — "
		                      "dock id already in use, deleting unregistered dock");
		delete g_dock;
		g_dock = nullptr;
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

	/* Request every output to stop, unconditionally — no is_running() gate
	 * (fix-round 2, 2026-07-07).  is_running() excludes Reconnecting, which
	 * left a window where a reconnecting controller reached
	 * EndpointRegistry::~EndpointRegistry() (and, via it, the reaper) without
	 * ever having its session detached here.  stop() is non-blocking and
	 * idempotent (see OutputController's detach-and-reaper rule) — this just
	 * kicks off a graceful RTMP stop before the registry destructor (and, via
	 * it, the ControllerReaper) does the real, potentially-blocking teardown
	 * below. */
	if (g_registry) {
		for (auto &ep : g_registry->all()) {
			auto ctrl = g_registry->controller_for(ep.id);
			if (ctrl)
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

	/* Dock must be explicitly removed: obs_frontend_add_dock_by_id() only
	 * hands ownership to Qt on success (see obs_module_load) — if it failed,
	 * g_dock was already deleted there and is nullptr here.  If it
	 * succeeded, the dock is still owned by OBS's frontend registry until
	 * explicitly removed; removing it before destroying g_registry avoids a
	 * dangling EndpointRegistry&/HealthSampler& reference inside the dock's
	 * child widgets during the teardown below. */
	if (g_dock)
		obs_frontend_remove_dock("avanatro_streammulticast");
	g_dock = nullptr;

	/* Destroying the registry enqueues every remaining OutputController to
	 * the reaper (see EndpointRegistry::~EndpointRegistry) instead of
	 * blocking here inside obs_output_release().  g_reaper->shutdown() then
	 * drains that queue and block-joins the reaper's worker thread — NEVER
	 * detach: a detached thread would keep running inside this DLL's code
	 * segment after Windows unloads it, a guaranteed use-after-free. */
	g_registry.reset();
	if (g_reaper)
		g_reaper->shutdown();
	g_reaper.reset();

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
