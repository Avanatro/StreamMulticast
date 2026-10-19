/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "OwnedCanvas.hpp"

#include "CanvasLookup.hpp"
#include "CanvasSceneNames.hpp"
#include "OutputController.hpp"
#include "../core/CanvasScaleRef.hpp"
#include "../core/EndpointRegistry.hpp"
#include "../core/OwnedCanvasPolicy.hpp"
#include "../plugin-support.h"

#include <obs.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <optional>
#include <utility>
#include <vector>

namespace smulti {

namespace {

constexpr const char *kDefaultSceneName = "V Main";
constexpr const char *kSceneSaveKey = "streammulticast_vertical_scene";
constexpr const char *kTransitionName = "StreamMulticast Vertical Transition";
constexpr uint32_t kFallbackWidth = 1080;
constexpr uint32_t kFallbackHeight = 1920;

/** The one place channel 0 of the owned canvas is read (strong ref or null). */
obs_source_t *channel_source(obs_canvas_t *canvas)
{
	return canvas ? obs_canvas_get_channel(canvas, 0) : nullptr;
}

/** Scene shown by a channel source: unwraps a transition (destination while it runs). New ref or null. */
obs_source_t *unwrap_scene(obs_source_t *channel)
{
	if (!channel)
		return nullptr;
	if (obs_source_get_type(channel) == OBS_SOURCE_TYPE_TRANSITION)
		return obs_transition_get_active_source(channel); // B while transitioning, else A
	return obs_source_get_ref(channel);
}

/** Exact: B is set by obs_transition_start and cleared only in obs_transition_stop (audio side included). */
bool transition_running(obs_source_t *transition)
{
	obs_source_t *b = transition ? obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B) : nullptr;
	const bool running = b != nullptr;
	obs_source_release(b);
	return running;
}

/**
 * obs_transition_set on one of our transitions, stopping a running one first: obs_transition_set clears
 * the sources but never calls the type's transition_stop, so an aborted stinger would keep its old fixed
 * duration and transition point for its next start (transition-stinger.c restart branch).
 */
void set_transition_stopped(obs_source_t *transition, obs_source_t *source)
{
	if (transition_running(transition))
		obs_transition_force_stop(transition);
	obs_transition_set(transition, source);
}

bool file_exists(const std::string &path)
{
	return !path.empty() && os_file_exists(path.c_str());
}

std::string source_uuid(obs_source_t *source)
{
	const char *u = source ? obs_source_get_uuid(source) : nullptr;
	return u ? u : "";
}

std::string source_name(obs_source_t *source)
{
	const char *n = source ? obs_source_get_name(source) : nullptr;
	return n ? n : "";
}

uint32_t frontend_duration_ms()
{
	const int d = obs_frontend_get_transition_duration();
	return d > 0 ? static_cast<uint32_t>(d) : 0u;
}

/** First scene of the canvas that is not removed (new ref) or null. */
bool first_live_scene(void *param, obs_source_t *scene)
{
	auto **out = static_cast<obs_source_t **>(param);
	if (*out || obs_source_removed(scene))
		return true;
	*out = obs_source_get_ref(scene);
	return true;
}

struct SceneScan {
	size_t count = 0;
	std::string first_name;
};

bool scan_scene(void *param, obs_source_t *scene)
{
	auto *scan = static_cast<SceneScan *>(param);
	if (scan->count++ == 0) {
		const char *name = obs_source_get_name(scene);
		scan->first_name = name ? name : "";
	}
	return true;
}

/** Snapshot via the frontend list (UI thread only; ruling F2). Releases every ref it takes. */
std::vector<CanvasInfo> snapshot_frontend_canvases()
{
	std::vector<CanvasInfo> out;
	obs_frontend_canvas_list list = {};
	obs_frontend_get_canvases(&list);
	for (size_t i = 0; i < list.canvases.num; ++i) {
		obs_canvas_t *c = list.canvases.array[i];
		const char *uuid = obs_canvas_get_uuid(c);
		const char *name = obs_canvas_get_name(c);
		CanvasInfo info;
		info.uuid = uuid ? uuid : "";
		info.name = name ? name : "";
		info.has_video = obs_canvas_has_video(c);
		info.is_main = (obs_canvas_get_flags(c) & static_cast<uint32_t>(MAIN)) != 0;
		out.push_back(std::move(info));
	}
	obs_frontend_canvas_list_free(&list);
	return out;
}

/** Main video settings with the vertical geometry applied. False when libobs has no video. */
bool build_vertical_ovi(obs_video_info &ovi)
{
	if (!obs_get_video_info(&ovi))
		return false;

	VideoGeometry main_geo;
	main_geo.base_width = ovi.base_width;
	main_geo.base_height = ovi.base_height;
	main_geo.output_width = ovi.output_width;
	main_geo.output_height = ovi.output_height;
	main_geo.fps_num = ovi.fps_num;
	main_geo.fps_den = ovi.fps_den;

	const VideoGeometry v = derive_vertical_geometry(main_geo);
	ovi.base_width = v.base_width;
	ovi.base_height = v.base_height;
	ovi.output_width = v.output_width;
	ovi.output_height = v.output_height;
	ovi.fps_num = v.fps_num;
	ovi.fps_den = v.fps_den;
	return true;
}

bool collect_scene_ref(void *param, obs_source_t *scene)
{
	obs_source_t *ref = obs_source_get_ref(scene);
	if (ref)
		static_cast<std::vector<obs_source_t *> *>(param)->push_back(ref);
	return true;
}

bool hold_item_source(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	obs_source_t *ref = obs_source_get_ref(obs_sceneitem_get_source(item));
	if (ref)
		static_cast<std::vector<obs_source_t *> *>(param)->push_back(ref);
	return true;
}

/**
 * Items whose saved scale_ref is unusable ({0,0}: scenes imported by the TASK-13 build, copied in a canvas
 * without video) get the canvas base size as scale_ref; items whose saved scale is {0,0} or not finite
 * (seen live on a copied Webcam item) get a usable scale (repair_scene_item_scales: {1,1} when bounded,
 * else recomputed). The scene then reloads its items from the patched settings (libobs scene_load reads
 * scale_ref and the scale). Idempotent: a scene without anything to repair is not reloaded.
 * UI thread only; refs are taken and released here.
 */
void repair_scale_refs(obs_canvas_t *canvas)
{
	obs_video_info ovi = {};
	if (!obs_canvas_get_video_info(canvas, &ovi) || ovi.base_width == 0 || ovi.base_height == 0)
		return;

	/* Collect first: obs_source_load must not run inside the canvas enumeration. */
	std::vector<obs_source_t *> scenes;
	obs_canvas_enum_scenes(canvas, collect_scene_ref, &scenes);

	for (obs_source_t *scene_src : scenes) {
		obs_scene_t *scene = obs_scene_from_source(scene_src);
		if (!scene || obs_source_is_group(scene_src))
			continue;
		obs_source_save(scene_src); // writes the current items into the settings (same object as below)
		obs_data_t *settings = obs_source_get_settings(scene_src);
		/* scale_ref first: the scale recompute divides by it. */
		const size_t repaired = repair_scene_scale_refs(settings, ovi.base_width, ovi.base_height);
		const size_t scales = repair_scene_item_scales(settings, ovi.base_width, ovi.base_height);
		if (repaired > 0 || scales > 0) {
			/* scene_load removes every item before re-adding it: keep the sources alive meanwhile (a
			 * source used only by this scene would otherwise be destroyed and its item lost), as the OBS
			 * frontend undo does. */
			std::vector<obs_source_t *> held;
			obs_scene_enum_items(scene, hold_item_source, &held);
			obs_source_load(scene_src);
			for (obs_source_t *s : held)
				obs_source_release(s);
			obs_log(LOG_INFO,
				"owned canvas: repaired scale_ref of %zu item(s) and scale of %zu item(s) in scene '%s' (%ux%u)",
				repaired, scales, obs_source_get_name(scene_src), ovi.base_width, ovi.base_height);
		}
		obs_data_release(settings);
	}
	for (obs_source_t *s : scenes)
		obs_source_release(s);
}

} // anonymous namespace

OwnedCanvas::~OwnedCanvas()
{
	release();
}

void OwnedCanvas::on_canvas_rename(void *param, calldata_t *cd)
{
	/* Any thread, possibly with libobs locks held: only update the mutex guarded link map. */
	auto *self = static_cast<OwnedCanvas *>(param);
	auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source || !obs_source_is_scene(source))
		return;
	const char *new_name = calldata_string(cd, "new_name");
	const char *prev_name = calldata_string(cd, "prev_name");
	if (new_name && prev_name)
		self->on_scene_renamed(prev_name, new_name);
}

void OwnedCanvas::on_canvas_source_remove(void *param, calldata_t *cd)
{
	/* Any thread: only post. The wrapper holds its scenes, so libobs no longer drops a removed scene from
	 * channel 0 by itself (obs-view.c); hand over on the UI thread. */
	auto *self = static_cast<OwnedCanvas *>(param);
	auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source || !obs_source_is_scene(source) || self->m_shutting_down.load() || !self->m_post)
		return;
	self->m_post([self]() { self->repair_removed_scene(); });
}

void OwnedCanvas::disconnect_canvas_signals(obs_weak_canvas_t *weak)
{
	/* Only touch the handler while the canvas is provably alive; a dead canvas took its handler along.
	 * From the canvas_remove signal the remover still holds its own ref, so the release is not the last. */
	obs_canvas_t *c = obs_weak_canvas_get_canvas(weak);
	if (!c)
		return;
	signal_handler_t *handler = obs_canvas_get_signal_handler(c);
	if (handler) {
		signal_handler_disconnect(handler, "source_rename", &OwnedCanvas::on_canvas_rename, this);
		signal_handler_disconnect(handler, "source_remove", &OwnedCanvas::on_canvas_source_remove, this);
	}
	obs_canvas_release(c);
}

void OwnedCanvas::remember(obs_canvas_t *canvas)
{
	obs_weak_canvas_t *weak = obs_canvas_get_weak_canvas(canvas);
	const char *uuid = obs_canvas_get_uuid(canvas);

	obs_weak_canvas_t *old = nullptr;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		old = m_weak;
		m_weak = weak;
		m_uuid = uuid ? uuid : "";
	}
	if (old) {
		disconnect_canvas_signals(old); // also covers a reused canvas: never connect twice
		obs_weak_canvas_release(old);
	}

	/* Renames of owned-canvas scenes are only signalled on the canvas itself (libobs emits the
	 * global source_rename for main canvases only). */
	signal_handler_t *handler = obs_canvas_get_signal_handler(canvas);
	if (handler) {
		signal_handler_connect(handler, "source_rename", &OwnedCanvas::on_canvas_rename, this);
		signal_handler_connect(handler, "source_remove", &OwnedCanvas::on_canvas_source_remove, this);
	}
}

void OwnedCanvas::ensure()
{
	obs_video_info ovi = {};
	if (!build_vertical_ovi(ovi)) {
		obs_log(LOG_WARNING, "owned canvas: no main video settings yet, skipping");
		return;
	}

	const OwnedCanvasDecision decision = decide_owned_canvas(snapshot_frontend_canvases());

	obs_canvas_t *canvas = nullptr; // strong ref we own
	switch (decision.action) {
	case OwnedCanvasAction::Create:
		canvas = obs_frontend_add_canvas(kOwnedVerticalCanvasName, &ovi, PROGRAM);
		if (!canvas) {
			obs_log(LOG_ERROR, "owned canvas: could not create '%s'", kOwnedVerticalCanvasName);
			return;
		}
		obs_log(LOG_INFO, "owned canvas: created '%s' (%ux%u)", kOwnedVerticalCanvasName, ovi.base_width,
			ovi.base_height);
		break;

	case OwnedCanvasAction::ReuseAndResetVideo:
		canvas = acquire_canvas(decision.uuid);
		if (!canvas)
			return;
		if (obs_canvas_reset_video(canvas, &ovi)) {
			obs_log(LOG_INFO, "owned canvas: reused '%s' and rebuilt its video (%ux%u)",
				kOwnedVerticalCanvasName, ovi.base_width, ovi.base_height);
		} else {
			obs_log(LOG_WARNING,
				"owned canvas: '%s' has no video and reset_video was refused "
				"(OBS video active?), it stays unusable until the next collection load",
				kOwnedVerticalCanvasName);
		}
		break;

	case OwnedCanvasAction::Reuse:
		canvas = acquire_canvas(decision.uuid);
		if (!canvas)
			return;
		obs_log(LOG_INFO, "owned canvas: reused '%s'", kOwnedVerticalCanvasName);
		break;
	}

	remember(canvas);
	if (obs_canvas_has_video(canvas)) {
		repair_scale_refs(canvas);
		refresh_prepared_on(canvas); // every frontend transition prepared before it is used
		ensure_active_scene(canvas);
	}
	obs_canvas_release(canvas);
}

void OwnedCanvas::ensure_active_scene(obs_canvas_t *canvas)
{
	SceneScan scan;
	obs_canvas_enum_scenes(canvas, scan_scene, &scan);

	if (scan.count > 0) {
		/* A restored canvas has its scenes but no channel assignment (libobs
		 * does not persist it). Only fill an empty one. */
		obs_source_t *current = active_scene_ref(canvas);
		if (current) {
			obs_source_release(current);
			return;
		}

		std::string wanted;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			wanted = m_scene_name;
		}
		obs_scene_t *scene = wanted.empty() ? nullptr : obs_canvas_get_scene_by_name(canvas, wanted.c_str());
		if (!scene)
			scene = obs_canvas_get_scene_by_name(canvas, scan.first_name.c_str());
		if (!scene)
			return;
		switch_scene(canvas, obs_scene_get_source(scene), SceneSwitch::Instant);
		obs_log(LOG_INFO, "owned canvas: restored scene '%s' on channel 0",
			obs_source_get_name(obs_scene_get_source(scene)));
		obs_scene_release(scene);
		return;
	}

	obs_scene_t *scene = obs_canvas_scene_create(canvas, kDefaultSceneName);
	if (!scene) {
		obs_log(LOG_WARNING, "owned canvas: could not create scene '%s'", kDefaultSceneName);
		return;
	}
	switch_scene(canvas, obs_scene_get_source(scene), SceneSwitch::Instant);
	obs_scene_release(scene); // the canvas holds its own refs (SCENE_REF), the transition holds one too
	obs_log(LOG_INFO, "owned canvas: created empty scene '%s'", kDefaultSceneName);
}

void OwnedCanvas::on_collection_changing(EndpointRegistry &registry)
{
	const std::string owned = uuid();
	if (!owned.empty()) {
		std::vector<std::pair<std::string, std::string>> bound;
		for (const auto &ep : registry.all()) {
			auto ctrl = registry.controller_for(ep.id);
			if (ctrl)
				bound.emplace_back(ep.id, ctrl->bound_canvas_uuid());
		}
		for (const auto &id : endpoints_to_stop_on_canvas_removed(bound, owned)) {
			auto ctrl = registry.controller_for(id);
			if (ctrl) {
				obs_log(LOG_INFO,
					"owned canvas: scene collection changing, stopping endpoint '%s' bound to '%s'",
					id.c_str(), kOwnedVerticalCanvasName);
				ctrl->stop();
			}
		}
	}
	release();
}

void OwnedCanvas::release()
{
	release_transition(); // needs the weak ref to clear channel 0 first
	obs_weak_canvas_t *weak = nullptr;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		weak = m_weak;
		m_weak = nullptr;
		m_uuid.clear();
	}
	if (weak) {
		disconnect_canvas_signals(weak);
		obs_weak_canvas_release(weak);
	}
}

obs_canvas_t *OwnedCanvas::get_ref() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	obs_canvas_t *c = m_weak ? obs_weak_canvas_get_canvas(m_weak) : nullptr;
	/* A removed canvas can stay alive while an endpoint still pins it. The release below is never
	 * the last one: plugin-main drops m_weak on the libobs canvas_remove signal, while the remover
	 * still holds its own ref. */
	if (c && obs_canvas_removed(c)) {
		obs_canvas_release(c);
		return nullptr;
	}
	return c;
}

void OwnedCanvas::on_save_load(obs_data_t *data, bool saving)
{
	if (!data)
		return;
	if (!saving) {
		const char *name = obs_data_get_string(data, kSceneSaveKey);
		SceneLinkSettings links = scene_link_load(data);
		VerticalTransitionSettings transition = vertical_transition_load(data);
		std::lock_guard<std::mutex> lock(m_mutex);
		m_scene_name = name ? name : "";
		m_links = std::move(links);
		m_transition_settings = std::move(transition);
		return;
	}

	obs_canvas_t *c = get_ref();
	std::string name;
	if (c) {
		name = active_scene_name(c); // the destination while a transition runs
		obs_canvas_release(c);
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!name.empty())
		m_scene_name = name;
	if (!m_scene_name.empty())
		obs_data_set_string(data, kSceneSaveKey, m_scene_name.c_str());
	scene_link_save(data, m_links);
	vertical_transition_save(data, m_transition_settings);
}

std::string OwnedCanvas::uuid() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_uuid;
}

SceneLinkSettings OwnedCanvas::link_settings() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_links;
}

void OwnedCanvas::set_link_settings(const SceneLinkSettings &settings)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_links = settings;
}

void OwnedCanvas::on_scene_renamed(const std::string &old_name, const std::string &new_name)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	rename_scene_in_links(m_links, old_name, new_name);
}

void OwnedCanvas::follow_program_scene()
{
	std::string main_name;
	obs_source_t *program = obs_frontend_get_current_scene();
	if (program) {
		const char *n = obs_source_get_name(program);
		main_name = n ? n : "";
		obs_source_release(program);
	}
	follow_scene(main_name);
}

void OwnedCanvas::follow_scene(const std::string &main_name)
{
	const SceneLinkSettings settings = link_settings();
	if (!settings.enabled || main_name.empty())
		return;

	obs_canvas_t *canvas = get_ref();
	if (!canvas)
		return;

	if (obs_canvas_has_video(canvas)) {
		const std::vector<std::string> verticals = canvas_scene_names(canvas);
		const std::optional<std::string> target = resolve_linked_scene(main_name, settings, verticals);
		/* Already active or already the destination of the running vertical transition: nothing to do. */
		if (scene_changed_needs_catch_up(target, active_scene_name(canvas))) {
			obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas, target->c_str());
			if (scene) {
				switch_scene(canvas, obs_scene_get_source(scene), SceneSwitch::Instant);
				obs_scene_release(scene);
				obs_log(LOG_INFO, "scene link: \"%s\" -> \"%s\"", main_name.c_str(), target->c_str());
			}
		}
	}
	obs_canvas_release(canvas);
}

/* ---- vertical scene transition (TASK-31) ------------------------------------ */

obs_source_t *OwnedCanvas::active_scene_ref(obs_canvas_t *canvas)
{
	obs_source_t *channel = channel_source(canvas);
	obs_source_t *scene = unwrap_scene(channel);
	obs_source_release(channel);
	if (scene && obs_source_removed(scene)) {
		/* Removed, the hand-off (repair_removed_scene) is queued: never edit or save it. */
		obs_source_release(scene);
		return nullptr;
	}
	return scene;
}

std::string OwnedCanvas::active_scene_name(obs_canvas_t *canvas)
{
	std::string out;
	obs_source_t *scene = active_scene_ref(canvas);
	if (scene) {
		out = source_name(scene);
		obs_source_release(scene);
	}
	return out;
}

bool OwnedCanvas::is_owned(obs_canvas_t *canvas) const
{
	const char *u = canvas ? obs_canvas_get_uuid(canvas) : nullptr;
	return u && *u && uuid() == u;
}

VerticalTransitionSettings OwnedCanvas::transition_settings() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_transition_settings;
}

void OwnedCanvas::set_transition_settings(const VerticalTransitionSettings &settings)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_transition_settings = settings;
}

void OwnedCanvas::set_active_scene_listener(std::function<void()> listener)
{
	m_active_listener = std::move(listener);
}

/* ---- own transitions ---------------------------------------------------------- */

obs_source_t *OwnedCanvas::create_wrapper(obs_canvas_t *canvas, const std::string &id)
{
	if (m_shutting_down.load())
		return nullptr;
	obs_source_t *w = obs_source_create_private(id.c_str(), kTransitionName, nullptr);
	if (!w) {
		obs_log(LOG_WARNING, "vertical transition: could not create a '%s' transition", id.c_str());
		return nullptr;
	}
	obs_video_info ovi = {};
	const bool have_size = obs_canvas_get_video_info(canvas, &ovi) && ovi.base_width && ovi.base_height;
	obs_transition_set_size(w, have_size ? ovi.base_width : kFallbackWidth, have_size ? ovi.base_height : kFallbackHeight);
	obs_transition_set_alignment(w, OBS_ALIGN_CENTER);
	/* R1: the main mix already carries the stinger sound; the canvas mixes its channel audio into it too. The
	 * wrapper stays in audio rendering (a transition only stops after its audio side stopped). */
	obs_source_set_muted(w, true);
	signal_handler_connect(obs_source_get_signal_handler(w), "transition_stop", &OwnedCanvas::on_transition_stop, this);
	return w;
}

void OwnedCanvas::destroy_wrapper(obs_source_t *wrapper)
{
	if (!wrapper)
		return;
	/* Disconnect waits for an in-flight callback (signal dispatch holds the handler mutex). */
	signal_handler_disconnect(obs_source_get_signal_handler(wrapper), "transition_stop",
				  &OwnedCanvas::on_transition_stop, this);
	set_transition_stopped(wrapper, nullptr); // drop the scene refs now
	obs_source_release(wrapper);         // UI thread
}

bool OwnedCanvas::is_own_wrapper(obs_source_t *source) const
{
	if (!source)
		return false;
	if (source == m_base)
		return true;
	for (const Prepared &p : m_prepared) {
		if (p.wrapper == source)
			return true;
	}
	return false;
}

void OwnedCanvas::install(obs_canvas_t *canvas, obs_source_t *wrapper)
{
	obs_source_t *channel = channel_source(canvas);
	if (channel == wrapper) {
		obs_source_release(channel);
		return;
	}
	/* The new one first shows what the canvas shows now (destination of a running transition). */
	obs_source_t *keep = unwrap_scene(channel);
	if (keep && obs_source_removed(keep)) {
		obs_source_release(keep);
		keep = nullptr;
	}
	if (keep)
		set_transition_stopped(wrapper, keep);
	obs_canvas_set_channel(canvas, 0, wrapper);
	if (is_own_wrapper(channel))
		set_transition_stopped(channel, nullptr); // kept prepared, holds no scene while off channel 0
	obs_source_release(keep);
	obs_source_release(channel);
}

void OwnedCanvas::retire_wrapper(obs_canvas_t *canvas, obs_source_t *wrapper)
{
	if (!wrapper)
		return;
	if (canvas && wrapper != m_base) {
		obs_source_t *channel = channel_source(canvas);
		const bool on_channel = channel == wrapper;
		obs_source_release(channel);
		if (on_channel && ensure_base(canvas))
			install(canvas, m_base);
	}
	destroy_wrapper(wrapper);
}

obs_source_t *OwnedCanvas::ensure_base(obs_canvas_t *canvas)
{
	if (!m_base)
		m_base = create_wrapper(canvas, kFadeTransitionId);
	if (!m_base)
		m_base = create_wrapper(canvas, kCutTransitionId);
	return m_base;
}

obs_source_t *OwnedCanvas::channel_wrapper(obs_canvas_t *canvas)
{
	obs_source_t *channel = channel_source(canvas);
	const bool ours = is_own_wrapper(channel);
	obs_source_release(channel); // still alive: we hold our own reference
	if (ours)
		return channel;
	obs_source_t *base = ensure_base(canvas);
	if (base)
		install(canvas, base);
	return base;
}

const OwnedCanvas::Prepared *OwnedCanvas::find_prepared(const std::string &uuid, const std::string &name) const
{
	for (const Prepared &p : m_prepared) {
		if (!uuid.empty() ? p.uuid == uuid : (!name.empty() && p.name == name))
			return &p;
	}
	return nullptr;
}

void OwnedCanvas::refresh_prepared_on(obs_canvas_t *canvas)
{
	if (m_shutting_down.load())
		return;
	ensure_base(canvas);

	/* Each frontend transition as the "main" one: the start path then only picks by uuid. */
	VerticalTransitionSettings follow = transition_settings();
	follow.mode = VerticalTransitionMode::Follow;
	const uint32_t duration = frontend_duration_ms();

	std::vector<Prepared> next;
	obs_frontend_source_list list = {};
	obs_frontend_get_transitions(&list);
	for (size_t i = 0; i < list.sources.num; ++i) {
		obs_source_t *tr = list.sources.array[i];
		const std::string uuid = source_uuid(tr);
		if (uuid.empty())
			continue;

		Prepared entry;
		for (auto it = m_prepared.begin(); it != m_prepared.end(); ++it) {
			if (it->uuid == uuid) {
				entry = std::move(*it);
				m_prepared.erase(it);
				break;
			}
		}
		entry.uuid = uuid;
		entry.name = source_name(tr);
		if (!entry.frontend) {
			/* The main transition's own edits (properties dialog, websocket) re-prepare it. */
			entry.frontend = obs_source_get_weak_source(tr);
			signal_handler_t *handler = obs_source_get_signal_handler(tr);
			signal_handler_connect(handler, "update", &OwnedCanvas::on_frontend_update, this);
			/* The main switch starts this transition: start the vertical one at the same moment. */
			signal_handler_connect(handler, "transition_start", &OwnedCanvas::on_frontend_transition_start,
					       this);
		}

		TransitionDesc desc;
		const char *id = obs_source_get_id(tr);
		desc.id = id ? id : "";
		desc.name = entry.name;
		obs_data_t *settings = obs_source_get_settings(tr);
		desc.settings = settings;
		desc.duration_ms = duration;
		entry.plan = decide_vertical_transition(follow, desc, nullptr, file_exists);
		obs_data_release(settings);

		if (entry.plan.fade_fallback && !entry.warned) {
			obs_log(LOG_WARNING,
				"vertical transition: no vertical video for stinger '%s', the vertical canvas fades over %u ms instead",
				entry.name.c_str(), entry.plan.duration_ms);
			entry.warned = true;
		} else if (!entry.plan.fade_fallback) {
			entry.warned = false;
		}

		const bool own = !entry.plan.instant && entry.plan.id != kFadeTransitionId;
		if (!own || (entry.wrapper && entry.wrapper_id != entry.plan.id)) {
			retire_wrapper(canvas, entry.wrapper);
			entry.wrapper = nullptr;
			entry.wrapper_id.clear();
			entry.applied_json.clear();
		}
		if (own && !entry.wrapper) {
			entry.wrapper = create_wrapper(canvas, entry.plan.id);
			entry.wrapper_id = entry.wrapper ? entry.plan.id : std::string();
		}
		/* Only on change (a stinger rebuilds its media on every update) and never while it runs: the
		 * transition_stop signal brings us back here afterwards. */
		if (entry.wrapper && entry.applied_json != entry.plan.settings_json && !transition_running(entry.wrapper)) {
			obs_data_t *data = obs_data_create_from_json(entry.plan.settings_json.c_str());
			if (data) {
				obs_source_update(entry.wrapper, data);
				obs_data_release(data);
			}
			entry.applied_json = entry.plan.settings_json;
		}
		next.push_back(std::move(entry));
	}
	obs_frontend_source_list_free(&list);

	/* Frontend transitions that are gone: their prepared transitions go too. */
	std::vector<Prepared> gone;
	gone.swap(m_prepared);
	m_prepared = std::move(next);
	for (Prepared &old : gone) {
		disconnect_frontend(old.frontend);
		retire_wrapper(canvas, old.wrapper);
	}

	/* Channel 0 always holds one of ours once the canvas has video. */
	channel_wrapper(canvas);
}

void OwnedCanvas::start_switch(obs_canvas_t *canvas, obs_source_t *scene, const Prepared *entry, bool instant,
			       uint32_t main_duration_ms)
{
	if (!scene)
		return;
	obs_source_t *wrapper = nullptr;
	if (instant || (entry && entry->plan.instant)) {
		wrapper = channel_wrapper(canvas);
		if (wrapper)
			set_transition_stopped(wrapper, scene);
	} else {
		uint32_t duration = main_duration_ms;
		if (entry && entry->wrapper && entry->applied_json == entry->plan.settings_json) {
			wrapper = entry->wrapper; // prepared: only swap and start, never create or update here
		} else {
			/* The base fade: the plan itself (a fade, or the stinger fade fallback) or, when nothing is
			 * prepared yet, a fade for this one switch (prepared for the next one). */
			wrapper = ensure_base(canvas);
			if (entry && entry->plan.fade_fallback)
				duration = entry->plan.duration_ms;
			if (!entry || (entry->plan.id != kFadeTransitionId))
				queue_refresh();
		}
		if (duration == 0)
			duration = 300;
		if (wrapper) {
			install(canvas, wrapper);
			/* Returns false only when the scene is already shown and nothing runs: nothing to do then. */
			obs_transition_start(wrapper, OBS_TRANSITION_MODE_AUTO, duration, scene);
		}
	}
	if (!wrapper)
		obs_canvas_set_channel(canvas, 0, scene); // no transition could be created: show the scene directly
	if (m_active_listener)
		m_active_listener();
}

bool OwnedCanvas::switch_scene(obs_canvas_t *canvas, obs_source_t *scene, SceneSwitch how)
{
	if (!canvas || !scene || !is_owned(canvas) || m_shutting_down.load())
		return false;
	const VerticalTransitionSettings settings = transition_settings();
	if (how == SceneSwitch::Instant || settings.mode == VerticalTransitionMode::Cut) {
		start_switch(canvas, scene, nullptr, true, 0);
		return true;
	}
	const Prepared *entry = nullptr;
	if (settings.mode == VerticalTransitionMode::Custom)
		entry = find_prepared("", settings.custom_name);
	if (!entry) {
		obs_source_t *current = obs_frontend_get_current_transition();
		entry = find_prepared(source_uuid(current), "");
		obs_source_release(current);
	}
	start_switch(canvas, scene, entry, false, frontend_duration_ms());
	return true;
}

void OwnedCanvas::on_main_transition_start(const MainTransitionStart &start)
{
	if (m_shutting_down.load())
		return;
	const SceneLinkSettings links = link_settings();
	if (!links.enabled || start.dest_scene.empty())
		return;
	obs_canvas_t *canvas = get_ref();
	if (!canvas)
		return;

	if (obs_canvas_has_video(canvas)) {
		const std::optional<std::string> target =
			resolve_linked_scene(start.dest_scene, links, canvas_scene_names(canvas));
		if (target && active_scene_name(canvas) != *target) {
			obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas, target->c_str());
			if (scene) {
				const VerticalTransitionSettings settings = transition_settings();
				const bool cut = settings.mode == VerticalTransitionMode::Cut ||
						 (start.id == kCutTransitionId &&
						  settings.mode == VerticalTransitionMode::Follow);
				const Prepared *entry = nullptr;
				uint32_t duration = main_transition_duration_ms(start.name, start.override_name,
										start.override_duration_ms,
										obs_frontend_get_transition_duration());
				if (settings.mode == VerticalTransitionMode::Custom) {
					entry = find_prepared("", settings.custom_name);
					if (entry)
						duration = frontend_duration_ms();
					else
						obs_log(LOG_WARNING,
							"vertical transition: custom transition '%s' not found, following main",
							settings.custom_name.c_str());
				}
				if (!entry)
					entry = find_prepared(start.uuid, start.name);
				start_switch(canvas, obs_scene_get_source(scene), entry, cut, duration);
				const char *type = cut || (entry && entry->plan.instant) ? kCutTransitionId
						   : (entry && entry->wrapper)          ? entry->plan.id.c_str()
											: kFadeTransitionId;
				const uint32_t shown_ms = cut || (entry && entry->plan.instant) ? 0u
							  : (entry && entry->plan.fade_fallback) ? entry->plan.duration_ms
												  : duration;
				obs_log(LOG_INFO, "vertical transition: '%s' %u ms -> '%s' (main '%s' -> '%s')", type, shown_ms,
					target->c_str(), start.name.c_str(), start.dest_scene.c_str());
				obs_scene_release(scene);
			}
		}
	}
	obs_canvas_release(canvas);
}

void OwnedCanvas::repair_removed_scene()
{
	if (m_shutting_down.load())
		return;
	obs_canvas_t *canvas = get_ref();
	if (!canvas)
		return;
	obs_source_t *channel = channel_source(canvas);
	if (channel && obs_source_get_type(channel) == OBS_SOURCE_TYPE_TRANSITION) {
		obs_source_t *a = obs_transition_get_source(channel, OBS_TRANSITION_SOURCE_A);
		obs_source_t *b = obs_transition_get_source(channel, OBS_TRANSITION_SOURCE_B);
		if ((a && obs_source_removed(a)) || (b && obs_source_removed(b))) {
			obs_source_t *target = nullptr;
			if (b && !obs_source_removed(b))
				target = obs_source_get_ref(b);
			else if (a && !obs_source_removed(a))
				target = obs_source_get_ref(a);
			else
				obs_canvas_enum_scenes(canvas, first_live_scene, &target);
			set_transition_stopped(channel, target); // instant; null when no scene is left
			obs_log(LOG_INFO, "owned canvas: active scene was removed, now showing '%s'",
				target ? source_name(target).c_str() : "(none)");
			obs_source_release(target);
			if (m_active_listener)
				m_active_listener();
		}
		obs_source_release(a);
		obs_source_release(b);
	}
	obs_source_release(channel);
	obs_canvas_release(canvas);
}

void OwnedCanvas::prepare_transition()
{
	if (m_shutting_down.load())
		return;
	obs_canvas_t *canvas = get_ref();
	if (!canvas)
		return;
	if (obs_canvas_has_video(canvas))
		refresh_prepared_on(canvas);
	obs_canvas_release(canvas);
}

void OwnedCanvas::release_transition()
{
	obs_canvas_t *canvas = nullptr;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		canvas = m_weak ? obs_weak_canvas_get_canvas(m_weak) : nullptr;
	}
	if (canvas) {
		obs_source_t *channel = channel_source(canvas);
		if (is_own_wrapper(channel))
			obs_canvas_set_channel(canvas, 0, nullptr);
		obs_source_release(channel);
		obs_canvas_release(canvas); // never the last one: see get_ref()
	}

	std::vector<Prepared> prepared;
	prepared.swap(m_prepared);
	for (Prepared &p : prepared) {
		disconnect_frontend(p.frontend);
		destroy_wrapper(p.wrapper); // UI thread, after channel 0 let go
	}
	obs_source_t *base = m_base;
	m_base = nullptr;
	destroy_wrapper(base);
}

void OwnedCanvas::shutdown_transitions()
{
	m_shutting_down = true; // ClearSceneData later emits TRANSITION_CHANGED and flushes queued posts
	release_transition();
}

void OwnedCanvas::queue_refresh()
{
	if (m_shutting_down.load() || !m_post || m_refresh_queued.exchange(true))
		return;
	m_post([this]() {
		m_refresh_queued = false;
		prepare_transition();
	});
}

void OwnedCanvas::on_transition_stop(void *param, calldata_t *)
{
	/* Graphics or audio thread: only post. Pending posts are dropped before this object is destroyed
	 * (unload releases the transitions, which disconnects this, and then deletes the UI context). */
	static_cast<OwnedCanvas *>(param)->queue_refresh();
}

void OwnedCanvas::on_frontend_update(void *param, calldata_t *)
{
	/* A frontend transition's settings changed (deferred update: graphics thread): re-prepare. */
	static_cast<OwnedCanvas *>(param)->queue_refresh();
}

void OwnedCanvas::disconnect_frontend(obs_weak_source_t *frontend)
{
	if (!frontend)
		return;
	/* Only touch the handler while the source provably lives; a destroyed source took its handler (and our
	 * registrations) along. Disconnect waits for an in-flight callback. */
	obs_source_t *fe = obs_weak_source_get_source(frontend);
	if (fe) {
		signal_handler_t *handler = obs_source_get_signal_handler(fe);
		signal_handler_disconnect(handler, "update", &OwnedCanvas::on_frontend_update, this);
		signal_handler_disconnect(handler, "transition_start", &OwnedCanvas::on_frontend_transition_start, this);
		obs_source_release(fe);
	}
	obs_weak_source_release(frontend);
}

void OwnedCanvas::on_frontend_transition_start(void *param, calldata_t *cd)
{
	/* Runs on the thread that started the transition (OBSBasic::TransitionToScene: UI thread), inside
	 * obs_transition_start after the destination became OBS_TRANSITION_SOURCE_B. Only copy plain values and
	 * post; the vertical transition starts on the UI thread right after. */
	auto *self = static_cast<OwnedCanvas *>(param);
	auto *transition = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!transition || self->m_shutting_down.load() || !self->m_post)
		return;

	/* Only the program transition (an override transition is put on output 0 before it starts; studio
	 * mode's preview and other users of a transition are ignored). */
	obs_source_t *program_output = obs_get_output_source(0);
	const bool is_program = program_output == transition;
	obs_source_release(program_output);
	if (!is_program)
		return;

	obs_source_t *dest = obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B); // fade-to-black: null
	if (!dest)
		return;
	MainTransitionStart start;
	start.dest_scene = source_name(dest);
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
	start.id = id ? id : "";
	start.name = source_name(transition);
	start.uuid = source_uuid(transition);
	obs_data_t *settings = obs_source_get_settings(transition);
	if (settings) {
		const char *json = obs_data_get_json(settings);
		start.settings_json = json ? json : "";
		obs_data_release(settings);
	}
	self->m_post([self, start]() { self->on_main_transition_start(start); });
}

} // namespace smulti
