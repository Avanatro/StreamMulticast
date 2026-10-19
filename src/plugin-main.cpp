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
#include "core/LinkedStart.hpp"
#include "core/MainServiceTarget.hpp"
#include "pipeline/ControllerReaper.hpp"
#include "pipeline/HealthSampler.hpp"
#include "pipeline/OutputController.hpp"
#include "pipeline/OwnedCanvas.hpp"
#include "ui/MultistreamDock.hpp"
#include "ui/VerticalCanvasDock.hpp"

#include <QMainWindow>
#include <QAction>
#include <QPointer>
#include <atomic>
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
static std::unique_ptr<smulti::OwnedCanvas>      g_owned_canvas; /* created after the registry, destroyed before it */
static smulti::MultistreamDock *                 g_dock = nullptr; /* owned by Qt only if registration succeeded — see obs_module_load */
static std::atomic<bool> g_module_alive{false};
/* Qt may destroy the vertical dock on its own at OBS exit, so track it with a guarded pointer. */
static QPointer<smulti::VerticalCanvasDock>      g_vertical_dock;
static bool                                      g_vertical_dock_registered = false;

static constexpr const char *kVerticalDockId = "avanatro_streammulticast_vertical";

/* Barrier: after this returns no preview draw holds a canvas ref. */
static void flush_vertical_preview()
{
	if (g_vertical_dock)
		g_vertical_dock->flush_preview();
}

/* Scene list disconnects from the owned canvas signals (thread safe). Must precede any release of the canvas. */
static void detach_vertical_scene_signals()
{
	if (g_vertical_dock)
		g_vertical_dock->detach_scene_signals();
}

static void refresh_vertical_scenes()
{
	if (g_vertical_dock)
		g_vertical_dock->refresh_scenes();
}

static void refresh_vertical_links()
{
	if (g_vertical_dock)
		g_vertical_dock->refresh_links();
}

/* Rebuild the owned canvas, then put it on the scene linked to the current program scene. UI thread. */
static void ensure_owned_canvas_and_follow()
{
	if (!g_owned_canvas)
		return;
	g_owned_canvas->ensure();
	g_owned_canvas->follow_program_scene();
	refresh_vertical_scenes();
}

/* libobs global "source_rename" (any thread): keep explicit scene links pointing at renamed scenes. */
static void on_source_rename(void *, calldata_t *cd)
{
	if (!g_module_alive.load() || !g_owned_canvas)
		return;
	auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source || !obs_source_is_scene(source))
		return;
	const char *new_name = calldata_string(cd, "new_name");
	const char *prev_name = calldata_string(cd, "prev_name");
	if (new_name && prev_name)
		g_owned_canvas->on_scene_renamed(prev_name, new_name);
}

/* libobs "canvas_remove" (global signal, emitted synchronously by obs_canvas_remove BEFORE the
 * caller releases its own ref; libobs obs-canvas.c:560-572). Drop our weak ref so get_ref() returns
 * null, then wait for any in-flight draw, so the frontend's later release is the final one and runs
 * on the UI thread, never inside the graphics thread's draw. */
static void on_canvas_remove(void *, calldata_t *cd)
{
	if (!g_module_alive.load() || !g_owned_canvas)
		return;
	auto *canvas = static_cast<obs_canvas_t *>(calldata_ptr(cd, "canvas"));
	const char *uuid = canvas ? obs_canvas_get_uuid(canvas) : nullptr;
	if (!uuid || g_owned_canvas->uuid() != uuid)
		return;
	detach_vertical_scene_signals(); // before the canvas can go away
	g_owned_canvas->release();
	flush_vertical_preview();
}

/* The preview display must die before any canvas reference is released and before the widget goes. */
static void shutdown_vertical_preview()
{
	detach_vertical_scene_signals();
	if (g_vertical_dock)
		g_vertical_dock->shutdown_preview();
}


/* Runs on the UI thread (queued from the libobs video_reset signal). */
static void ensure_owned_canvas_task(void *)
{
	if (g_module_alive.load())
		ensure_owned_canvas_and_follow();
}

/* obs_reset_video() frees every canvas mix; rebuild ours after a main video
 * reset. The signal may fire on any thread, and the frontend canvas API is
 * UI-thread only, so hop to the UI thread. */
static void on_video_reset(void *, calldata_t *)
{
	if (g_module_alive.load())
		obs_queue_task(OBS_TASK_UI, ensure_owned_canvas_task, nullptr, false);
}

static void on_save_load(obs_data_t *data, bool saving, void *)
{
	if (g_owned_canvas)
		g_owned_canvas->on_save_load(data, saving);
}

/* -----------------------------------------------------------------------
 * OBS frontend-event handler
 * Reacts to main-stream start/stop to trigger "linked" endpoints.
 * ----------------------------------------------------------------------- */
static void on_frontend_event(enum obs_frontend_event event, void * /*private_data*/)
{
	if (!g_registry)
		return;

	if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		/* OBSBasic::closeEvent clears the canvases (dropping their refs) BEFORE EXIT is
		 * emitted, but after this event, so the display must go now. */
		shutdown_vertical_preview();
	} else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED ||
	    event == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
		ensure_owned_canvas_and_follow();
	} else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
		/* Program scene only (studio mode preview fires PREVIEW_SCENE_CHANGED, ignored). */
		if (g_owned_canvas)
			g_owned_canvas->follow_program_scene();
	} else if (event == OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED) {
		refresh_vertical_links();
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING) {
		detach_vertical_scene_signals(); // before the owned canvas reference is dropped
		if (g_owned_canvas)
			g_owned_canvas->on_collection_changing(*g_registry);
		flush_vertical_preview();
		refresh_vertical_scenes(); // clears the list: the canvas is gone until the next ensure()
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
		obs_log(LOG_INFO, "OBS main stream started — triggering linked endpoints");
		/* Skip endpoints that would stream to the same server+key as the main
		 * stream or as an earlier endpoint. Log the endpoint name only,
		 * never the stream key. */
		auto plan = smulti::plan_linked_start(g_registry->all(), smulti::read_main_service_target());
		for (const auto &skipped : plan.skipped_duplicates) {
			obs_log(LOG_WARNING, "Skipping endpoint '%s': same server and key as %s",
				skipped.name.c_str(),
				skipped.reason == smulti::DuplicateKind::MainService ? "the OBS main stream"
										     : "another endpoint");
		}
		for (auto &ep : plan.to_start) {
			auto ctrl = g_registry->controller_for(ep.id);
			if (ctrl && !ctrl->is_running())
				ctrl->start();
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
		shutdown_vertical_preview();
		if (g_owned_canvas)
			g_owned_canvas->release();
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

	/* 3b. Owned vertical canvas adapter — needs the registry to stop bound endpoints. */
	g_owned_canvas = std::make_unique<smulti::OwnedCanvas>();

	/* 4. Boot health sampler — starts background 2-Hz polling thread */
	g_health_sampler = std::make_unique<smulti::HealthSampler>(*g_registry);
	g_health_sampler->start();

	/* 5. Register frontend event handler (for linked-to-main-stream behaviour) */
	// AVANATRO-VERIFY: obs_frontend_add_event_callback signature — verify it exists in
	// obs-frontend-api.h for OBS 30+ (it does in OBS 30.x, but confirm 31.x didn't rename it).
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	obs_frontend_add_save_callback(on_save_load, nullptr);
	g_module_alive = true;
	signal_handler_connect(obs_get_signal_handler(), "video_reset", on_video_reset, nullptr);
	signal_handler_connect(obs_get_signal_handler(), "canvas_remove", on_canvas_remove, nullptr);
	signal_handler_connect(obs_get_signal_handler(), "source_rename", on_source_rename, nullptr);

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

	/* 7. Vertical canvas dock. Registered after the owned canvas adapter exists; the canvas
	 * itself may not exist yet (ensure() runs on FINISHED_LOADING) and the preview copes. */
	g_vertical_dock = new smulti::VerticalCanvasDock(*g_owned_canvas);
	g_vertical_dock_registered =
		obs_frontend_add_dock_by_id(kVerticalDockId, "StreamMulticast Vertical", g_vertical_dock.data());
	if (!g_vertical_dock_registered) {
		obs_log(LOG_WARNING, "vertical dock id already in use, deleting unregistered dock");
		delete g_vertical_dock.data();
		g_vertical_dock.clear();
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

	g_module_alive = false;
	signal_handler_disconnect(obs_get_signal_handler(), "video_reset", on_video_reset, nullptr);
	signal_handler_disconnect(obs_get_signal_handler(), "canvas_remove", on_canvas_remove, nullptr);
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", on_source_rename, nullptr);
	obs_frontend_remove_save_callback(on_save_load, nullptr);
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

	/* Destroy the preview display first: its draw callback uses g_owned_canvas. */
	shutdown_vertical_preview();

	/* Drop the owned-canvas reference before the registry goes away. */
	g_owned_canvas.reset();

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

	if (g_vertical_dock_registered)
		obs_frontend_remove_dock(kVerticalDockId);
	g_vertical_dock_registered = false;
	g_vertical_dock.clear();

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
