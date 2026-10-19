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

bool transition_running(obs_source_t *transition)
{
	const float t = obs_transition_get_time(transition);
	return t > 0.0f && t < 1.0f;
}

bool file_exists(const std::string &path)
{
	return !path.empty() && os_file_exists(path.c_str());
}

/** Current frontend transition as a TransitionDesc; *settings_out is a new ref (release it) or null. */
TransitionDesc current_frontend_transition(obs_data_t **settings_out)
{
	TransitionDesc desc;
	*settings_out = nullptr;
	obs_source_t *tr = obs_frontend_get_current_transition();
	if (tr) {
		const char *id = obs_source_get_id(tr);
		const char *name = obs_source_get_name(tr);
		desc.id = id ? id : "";
		desc.name = name ? name : "";
		*settings_out = obs_source_get_settings(tr);
		desc.settings = *settings_out;
		obs_source_release(tr);
	}
	const int duration = obs_frontend_get_transition_duration();
	desc.duration_ms = duration > 0 ? static_cast<uint32_t>(duration) : 0u;
	return desc;
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

void OwnedCanvas::disconnect_rename(obs_weak_canvas_t *weak)
{
	/* Only touch the handler while the canvas is provably alive; a dead canvas took its handler along.
	 * From the canvas_remove signal the remover still holds its own ref, so the release is not the last. */
	obs_canvas_t *c = obs_weak_canvas_get_canvas(weak);
	if (!c)
		return;
	signal_handler_t *handler = obs_canvas_get_signal_handler(c);
	if (handler)
		signal_handler_disconnect(handler, "source_rename", &OwnedCanvas::on_canvas_rename, this);
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
		disconnect_rename(old); // also covers a reused canvas: never connect twice
		obs_weak_canvas_release(old);
	}

	/* Renames of owned-canvas scenes are only signalled on the canvas itself (libobs emits the
	 * global source_rename for main canvases only). */
	signal_handler_t *handler = obs_canvas_get_signal_handler(canvas);
	if (handler)
		signal_handler_connect(handler, "source_rename", &OwnedCanvas::on_canvas_rename, this);
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
		prepare_transition_on(canvas); // transition on channel 0 with the type it will use next
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
		disconnect_rename(weak);
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
		if (target && active_scene_name(canvas) != *target) {
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
	return scene;
}

std::string OwnedCanvas::active_scene_name(obs_canvas_t *canvas)
{
	std::string out;
	obs_source_t *scene = active_scene_ref(canvas);
	if (scene) {
		const char *n = obs_source_get_name(scene);
		out = n ? n : "";
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

VerticalTransitionPlan OwnedCanvas::plan_for(const TransitionDesc &main) const
{
	const VerticalTransitionSettings settings = transition_settings();
	TransitionDesc custom;
	obs_data_t *custom_settings = nullptr;
	bool have_custom = false;
	if (settings.mode == VerticalTransitionMode::Custom && !settings.custom_name.empty()) {
		obs_frontend_source_list list = {};
		obs_frontend_get_transitions(&list);
		for (size_t i = 0; i < list.sources.num && !have_custom; ++i) {
			obs_source_t *tr = list.sources.array[i];
			const char *name = obs_source_get_name(tr);
			if (!name || settings.custom_name != name)
				continue;
			const char *id = obs_source_get_id(tr);
			custom.id = id ? id : "";
			custom.name = name;
			custom_settings = obs_source_get_settings(tr);
			custom.settings = custom_settings;
			const int duration = obs_frontend_get_transition_duration();
			custom.duration_ms = duration > 0 ? static_cast<uint32_t>(duration) : 0u;
			have_custom = true;
		}
		obs_frontend_source_list_free(&list);
		if (!have_custom)
			obs_log(LOG_WARNING, "vertical transition: custom transition '%s' not found, following main",
				settings.custom_name.c_str());
	}
	VerticalTransitionPlan plan =
		decide_vertical_transition(settings, main, have_custom ? &custom : nullptr, file_exists);
	if (custom_settings)
		obs_data_release(custom_settings);
	return plan;
}

VerticalTransitionPlan OwnedCanvas::plan_for_current_transition() const
{
	obs_data_t *settings = nullptr;
	const TransitionDesc main = current_frontend_transition(&settings);
	VerticalTransitionPlan plan = plan_for(main);
	if (settings)
		obs_data_release(settings);
	return plan;
}

obs_source_t *OwnedCanvas::ensure_transition(obs_canvas_t *canvas, const std::string &id)
{
	obs_source_t *channel = channel_source(canvas);
	if (m_transition && channel == m_transition && m_transition_id == id) {
		obs_source_release(channel);
		return m_transition;
	}

	obs_source_t *created = obs_source_create_private(id.c_str(), kTransitionName, nullptr);
	if (!created) {
		obs_log(LOG_WARNING, "vertical transition: could not create a '%s' transition", id.c_str());
		const bool ours = m_transition && channel == m_transition;
		obs_source_release(channel);
		return ours ? m_transition : nullptr; // keep the old type rather than none
	}

	obs_video_info ovi = {};
	const bool have_size = obs_canvas_get_video_info(canvas, &ovi) && ovi.base_width && ovi.base_height;
	obs_transition_set_size(created, have_size ? ovi.base_width : kFallbackWidth,
				have_size ? ovi.base_height : kFallbackHeight);
	obs_transition_set_alignment(created, OBS_ALIGN_CENTER);

	/* New one first, showing the scene the canvas shows now (destination of a running transition). */
	obs_source_t *keep = unwrap_scene(channel);
	if (keep)
		obs_transition_set(created, keep);
	obs_canvas_set_channel(canvas, 0, created);

	/* After a run, load what the next switch will use (a per-scene override may have changed the type). */
	signal_handler_connect(obs_source_get_signal_handler(created), "transition_stop", &OwnedCanvas::on_transition_stop,
			       this);

	obs_source_t *old = m_transition;
	m_transition = created; // our creation ref
	m_transition_id = id;
	m_applied_json.clear();
	drop_transition_ref(old); // UI thread; channel 0 already holds the new one
	obs_source_release(keep);
	obs_source_release(channel);
	obs_log(LOG_INFO, "vertical transition: '%s' transition on '%s'", id.c_str(), kOwnedVerticalCanvasName);
	return m_transition;
}

obs_source_t *OwnedCanvas::configure_transition(obs_canvas_t *canvas, const VerticalTransitionPlan &plan)
{
	obs_source_t *transition = ensure_transition(canvas, plan.id);
	if (!transition || m_transition_id != plan.id)
		return transition;
	if (plan.settings_json != m_applied_json) {
		/* Only on change: a stinger rebuilds its media source on every update. */
		obs_data_t *data = obs_data_create_from_json(plan.settings_json.c_str());
		if (data) {
			obs_source_update(transition, data);
			obs_data_release(data);
		}
		m_applied_json = plan.settings_json;
	}
	obs_transition_set_scale_type(transition,
				      plan.scale_aspect ? OBS_TRANSITION_SCALE_ASPECT : OBS_TRANSITION_SCALE_MAX_ONLY);
	return transition;
}

bool OwnedCanvas::apply_plan(obs_canvas_t *canvas, const VerticalTransitionPlan &plan, obs_source_t *scene)
{
	if (!scene)
		return false;
	obs_source_t *transition = nullptr;
	if (plan.instant) {
		transition = ensure_transition(canvas, m_transition ? m_transition_id : std::string(kCutTransitionId));
		if (transition)
			obs_transition_set(transition, scene);
	} else {
		transition = configure_transition(canvas, plan);
		/* Returns false only when the scene is already shown and nothing runs: nothing to do then. */
		if (transition)
			obs_transition_start(transition, OBS_TRANSITION_MODE_AUTO, plan.duration_ms, scene);
	}
	if (!transition) {
		/* No transition could be created: show the scene directly (active_scene_ref copes with both). */
		obs_canvas_set_channel(canvas, 0, scene);
	}
	if (m_active_listener)
		m_active_listener();
	return true;
}

bool OwnedCanvas::switch_scene(obs_canvas_t *canvas, obs_source_t *scene, SceneSwitch how)
{
	if (!canvas || !scene || !is_owned(canvas))
		return false;
	if (how == SceneSwitch::Instant)
		return apply_plan(canvas, VerticalTransitionPlan{}, scene);
	return apply_plan(canvas, plan_for_current_transition(), scene);
}

void OwnedCanvas::on_main_transition_start(const MainTransitionStart &start)
{
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
				obs_data_t *settings = obs_data_create_from_json(start.settings_json.c_str());
				TransitionDesc main;
				main.id = start.id;
				main.name = start.name;
				main.settings = settings;
				main.duration_ms = main_transition_duration_ms(start.name, start.override_name,
									       start.override_duration_ms,
									       obs_frontend_get_transition_duration());
				const VerticalTransitionPlan plan = plan_for(main);
				if (settings)
					obs_data_release(settings);
				apply_plan(canvas, plan, obs_scene_get_source(scene));
				obs_scene_release(scene);
				obs_log(LOG_INFO, "scene link: \"%s\" -> \"%s\" (%s, %u ms)", start.dest_scene.c_str(),
					target->c_str(), plan.instant ? "instant" : plan.id.c_str(), plan.duration_ms);
			}
		}
	}
	obs_canvas_release(canvas);
}

void OwnedCanvas::prepare_transition()
{
	obs_canvas_t *canvas = get_ref();
	if (!canvas)
		return;
	if (obs_canvas_has_video(canvas))
		prepare_transition_on(canvas);
	obs_canvas_release(canvas);
}

void OwnedCanvas::prepare_transition_on(obs_canvas_t *canvas)
{
	if (m_transition && transition_running(m_transition))
		return; // never swap or reload a running transition
	const VerticalTransitionPlan plan = plan_for_current_transition();
	if (plan.instant)
		ensure_transition(canvas, m_transition ? m_transition_id : std::string(kCutTransitionId));
	else
		configure_transition(canvas, plan);
}

void OwnedCanvas::release_transition()
{
	obs_source_t *transition = m_transition;
	m_transition = nullptr;
	m_transition_id.clear();
	m_applied_json.clear();
	if (!transition)
		return;

	obs_canvas_t *canvas = nullptr;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		canvas = m_weak ? obs_weak_canvas_get_canvas(m_weak) : nullptr;
	}
	if (canvas) {
		obs_source_t *channel = channel_source(canvas);
		if (channel == transition)
			obs_canvas_set_channel(canvas, 0, nullptr);
		obs_source_release(channel);
		obs_canvas_release(canvas); // never the last one: see get_ref()
	}
	obs_transition_set(transition, nullptr); // drop the scene refs now
	drop_transition_ref(transition);         // UI thread, after channel 0 let go of it
}

void OwnedCanvas::drop_transition_ref(obs_source_t *transition)
{
	if (!transition)
		return;
	/* Disconnect waits for an in-flight callback (signal dispatch holds the handler mutex). */
	signal_handler_disconnect(obs_source_get_signal_handler(transition), "transition_stop",
				  &OwnedCanvas::on_transition_stop, this);
	obs_source_release(transition);
}

void OwnedCanvas::on_transition_stop(void *param, calldata_t *)
{
	/* Graphics thread: only post. Pending posts are dropped before this object is destroyed (unload deletes
	 * the UI context first). */
	auto *self = static_cast<OwnedCanvas *>(param);
	if (self->m_post)
		self->m_post([self]() { self->prepare_transition(); });
}

} // namespace smulti
