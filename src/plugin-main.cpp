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
#include "pipeline/VerticalRecorder.hpp"
#ifdef SMULTI_STREAMLABS_TIKTOK
#include "pipeline/StreamlabsClient.hpp"
#endif
#include "ui/MultistreamDock.hpp"
#include "ui/VerticalCanvasDock.hpp"

#include <QMainWindow>
#include <QAction>
#include <QPointer>
#include <atomic>
#include <functional>
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
static std::unique_ptr<smulti::VerticalRecorder> g_vertical_recorder; /* shut down before g_owned_canvas goes, destroyed after the dock */
/* UI-thread context for calls posted from libobs threads (vertical recorder "stop" signal, hotkey).
 * Deleted in unload only after every poster is disconnected; deleting it drops pending posts. */
static QObject *                                 g_ui_context = nullptr;
#ifdef SMULTI_STREAMLABS_TIKTOK
/* Optional Streamlabs TikTok key provider. Posts results through post_to_ui, so it is shut down
 * (worker joined, open session ended) before g_ui_context is deleted. */
static std::unique_ptr<smulti::StreamlabsClient> g_streamlabs;
#endif
static obs_hotkey_id                             g_record_hotkey = OBS_INVALID_HOTKEY_ID;
static bool                                      g_record_settings_saved = false;
static smulti::MultistreamDock *                 g_dock = nullptr; /* owned by Qt only if registration succeeded — see obs_module_load */
static std::atomic<bool> g_module_alive{false};
/* Qt may destroy the vertical dock on its own at OBS exit, so track it with a guarded pointer. */
static QPointer<smulti::VerticalCanvasDock>      g_vertical_dock;
static bool                                      g_vertical_dock_registered = false;

static constexpr const char *kVerticalDockId = "avanatro_streammulticast_vertical";

/* Any thread: run `fn` on the UI thread (dropped when the context is already gone). */
static void post_to_ui(std::function<void()> fn)
{
	QObject *context = g_ui_context;
	if (context)
		QMetaObject::invokeMethod(context, std::move(fn), Qt::QueuedConnection);
}

/* Streamlabs TikTok: end every open session synchronously (5 s bound each). Idempotent. UI thread. */
static void shutdown_streamlabs()
{
#ifdef SMULTI_STREAMLABS_TIKTOK
	if (g_streamlabs)
		g_streamlabs->shutdown_and_end_blocking();
#endif
}

/* Stop endpoints that hold a key provider session or a pending key fetch (Streamlabs TikTok). */
static void stop_provider_endpoints()
{
	if (!g_registry)
		return;
	for (auto &ep : g_registry->all()) {
		auto ctrl = g_registry->controller_for(ep.id);
		if (ctrl && ctrl->has_provider_work())
			ctrl->stop();
	}
}

/* Forced, non-blocking stop of the vertical recording (canvas going away, shutdown). UI thread. */
static void stop_vertical_recording(const char *reason)
{
	if (g_vertical_recorder)
		g_vertical_recorder->stop_now(reason);
}

/* Frontend hotkey "StreamMulticast: Start/Stop vertical recording" (hotkey thread). */
static void on_record_hotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !g_module_alive.load())
		return;
	post_to_ui([]() {
		if (g_module_alive.load() && g_vertical_recorder)
			g_vertical_recorder->toggle();
	});
}

/* Hotkey binding and recording settings, written once on shutdown (and on every settings edit). */
static void save_record_settings_once()
{
	if (g_record_settings_saved || !g_vertical_recorder)
		return;
	g_vertical_recorder->save_settings_file();
	g_record_settings_saved = true;
}

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

/* libobs global "source_transition_start". obs_transition_start emits it after the destination is
 * set as OBS_TRANSITION_SOURCE_B (libobs obs-source-transition.c:351-368), i.e. BEFORE the
 * transition runs; the frontend's SCENE_CHANGED only follows at transition end (OBSBasic::
 * TransitionStopped), or at once for the force/cut path. It fires for whatever transition is on output
 * channel 0, so a per-scene override transition is covered (OBSBasic::OverrideTransition puts it there
 * first). Private sources never emit it, so the plugin's own vertical transition does not re-enter.
 * The callback runs on the thread that started the transition: it copies plain values (type, name,
 * settings json, destination scene and its override) and queues them to the UI thread, where the vertical
 * transition starts (same type, settings copy and duration; TASK-31). */
static void on_transition_start(void *, calldata_t *cd)
{
	if (!g_module_alive.load() || !g_owned_canvas)
		return;
	auto *transition = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!transition)
		return;

	obs_source_t *program_output = obs_get_output_source(0);
	const bool is_program = program_output == transition;
	obs_source_release(program_output);
	if (!is_program)
		return;

	obs_source_t *dest = obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B); // fade-to-black: null
	if (!dest)
		return;
	smulti::MainTransitionStart start;
	const char *dest_name = obs_source_get_name(dest);
	start.dest_scene = dest_name ? dest_name : "";
	/* Per-scene override, as OBSBasic::GetOverrideTransition / GetOverrideTransitionDuration read it. */
	obs_data_t *priv = obs_source_get_private_settings(dest);
	if (priv) {
		const char *ov = obs_data_get_string(priv, "transition");
		start.override_name = ov ? ov : "";
		start.override_duration_ms = obs_data_has_user_value(priv, "transition_duration")
						     ? obs_data_get_int(priv, "transition_duration")
						     : 300;
		obs_data_release(priv);
	}
	obs_source_release(dest);
	if (start.dest_scene.empty())
		return;

	const char *id = obs_source_get_id(transition);
	const char *name = obs_source_get_name(transition);
	start.id = id ? id : "";
	start.name = name ? name : "";
	obs_data_t *settings = obs_source_get_settings(transition);
	if (settings) {
		const char *json = obs_data_get_json(settings);
		start.settings_json = json ? json : "";
		obs_data_release(settings);
	}

	post_to_ui([start]() {
		if (g_module_alive.load() && g_owned_canvas)
			g_owned_canvas->on_main_transition_start(start);
	});
}

static void disconnect_transition_start()
{
	signal_handler_disconnect(obs_get_signal_handler(), "source_transition_start", on_transition_start, nullptr);
}

/* Rebuild the owned canvas; optionally put it on the scene linked to the current program scene
 * (only on collection load/change: a profile change or video reset must not undo a manual Set active). */
static void ensure_owned_canvas_and_follow(bool follow)
{
	if (!g_owned_canvas)
		return;
	g_owned_canvas->ensure();
	if (follow)
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
	stop_vertical_recording("vertical canvas removed"); // its canvas ref is released last, on the reaper
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
		ensure_owned_canvas_and_follow(false);
}

/* obs_reset_video() frees every canvas mix; rebuild ours after a main video
 * reset. The signal may fire on any thread, and the frontend canvas API is
 * UI-thread only, so hop to the UI thread. */
static void on_video_reset(void *, calldata_t *)
{
	if (g_module_alive.load())
		obs_queue_task(OBS_TASK_UI, ensure_owned_canvas_task, nullptr, false);
}

/* Synchronous config write, only when something changed since the last write, so the three
 * shutdown call sites write at most once. Atomic (temp file + rename) inside ConfigStore. */
static void flush_config()
{
	if (g_config_store && g_registry)
		g_config_store->save_if_dirty(g_registry->all());
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
		/* First shutdown signal OBS gives us. Flush the config now: obs_module_unload comes
		 * after OBSBasic::ClearSceneData, which has crashed OBS on exit (2026-10-19). */
		flush_config();
		save_record_settings_once();
		stop_vertical_recording("OBS is shutting down");
		disconnect_transition_start();
		/* OBSBasic::closeEvent clears the canvases (dropping their refs) BEFORE EXIT is
		 * emitted, but after this event, so the display must go now. */
		shutdown_vertical_preview();
		/* The vertical transition holds vertical scene refs: let go before the frontend clears the scenes. */
		if (g_owned_canvas)
			g_owned_canvas->release_transition();
	} else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
		ensure_owned_canvas_and_follow(true);
	} else if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
		ensure_owned_canvas_and_follow(false);
	} else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
		/* Authoritative: the transition is over (or it was a cut / studio-mode swap / missed signal).
		 * Put the canvas on the linked scene of the now-current program scene, instantly; a no-op when
		 * it is already there or the vertical transition already runs towards it. Studio preview fires
		 * another event. */
		if (g_owned_canvas)
			g_owned_canvas->follow_program_scene();
	} else if (event == OBS_FRONTEND_EVENT_TRANSITION_CHANGED ||
		   event == OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED) {
		/* Load the next transition (a stinger video) ahead of its first use, and list it in the Links tab. */
		if (g_owned_canvas)
			g_owned_canvas->prepare_transition();
		if (event == OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED)
			refresh_vertical_links();
	} else if (event == OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED) {
		refresh_vertical_links();
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
		if (g_vertical_recorder)
			g_vertical_recorder->on_main_recording_started();
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPING) {
		if (g_vertical_recorder)
			g_vertical_recorder->on_main_recording_stopping();
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING) {
		stop_provider_endpoints(); // ends a Streamlabs TikTok session (TASK-28)
		stop_vertical_recording("scene collection changing"); // before the owned canvas reference goes
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
				/* has_provider_work: a pending key fetch or open provider session is ended too */
				if (ctrl && (ctrl->is_running() || ctrl->has_provider_work()))
					ctrl->stop();
			}
		}
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		/* Graceful shutdown — stop all outputs before OBS tears down */
		obs_log(LOG_INFO, "OBS exit — stopping all StreamMulticast outputs");
		flush_config();
		for (auto &ep : g_registry->all()) {
			auto ctrl = g_registry->controller_for(ep.id);
			if (ctrl && (ctrl->is_running() || ctrl->has_provider_work()))
				ctrl->stop();
		}
		shutdown_streamlabs(); // after the stops above queued their session ends
		stop_vertical_recording("OBS exit");
		disconnect_transition_start();
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

	/* 3c. Vertical recording of the owned canvas; its teardown jobs run on the reaper. */
	g_ui_context = new QObject(); // UI thread
#ifdef SMULTI_STREAMLABS_TIKTOK
	g_streamlabs = std::make_unique<smulti::StreamlabsClient>(post_to_ui);
	smulti::set_streamlabs_client(g_streamlabs.get());
#endif
	g_owned_canvas->set_ui_poster(post_to_ui); // vertical transition: reload after each run
	g_vertical_recorder = std::make_unique<smulti::VerticalRecorder>(*g_owned_canvas, *g_reaper, post_to_ui);
	g_record_hotkey = obs_hotkey_register_frontend("StreamMulticast.VerticalRecord",
						       "StreamMulticast: Start/Stop vertical recording", on_record_hotkey,
						       nullptr);
	if (g_record_hotkey != OBS_INVALID_HOTKEY_ID)
		g_vertical_recorder->set_hotkey(g_record_hotkey);

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
	signal_handler_connect(obs_get_signal_handler(), "source_transition_start", on_transition_start, nullptr);

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
	g_vertical_dock = new smulti::VerticalCanvasDock(*g_owned_canvas, *g_vertical_recorder);
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
	disconnect_transition_start();
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

	/* Vertical recording: save the hotkey binding while it is registered, then no libobs thread can post
	 * anymore (hotkey unregistered, output "stop" signal disconnected by shutdown), then drop pending posts.
	 * The recorder object itself lives until the dock is gone; its reaper jobs drain in g_reaper->shutdown(). */
	save_record_settings_once();
	if (g_record_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(g_record_hotkey);
	g_record_hotkey = OBS_INVALID_HOTKEY_ID;
	if (g_vertical_recorder)
		g_vertical_recorder->shutdown();
	/* Streamlabs: the stops above queued their session ends; run them now, join the worker,
	 * then no thread can post anymore. Controllers see a null client from here on. */
	shutdown_streamlabs();
#ifdef SMULTI_STREAMLABS_TIKTOK
	smulti::set_streamlabs_client(nullptr);
	g_streamlabs.reset();
#endif
	delete g_ui_context;
	g_ui_context = nullptr;

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
	flush_config();

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

	/* The dock referenced the recorder. After shutdown() it refuses start() and never touches the owned canvas. */
	g_vertical_recorder.reset();

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
