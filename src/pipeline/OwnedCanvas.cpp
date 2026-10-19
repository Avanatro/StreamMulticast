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

#include <optional>
#include <utility>
#include <vector>

namespace smulti {

namespace {

constexpr const char *kDefaultSceneName = "V Main";
constexpr const char *kSceneSaveKey = "streammulticast_vertical_scene";

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
 * without video) get the canvas base size as scale_ref; the scene then reloads its items from the patched
 * settings (libobs scene_load reads scale_ref). Idempotent: a scene without a broken ref is not reloaded.
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
		const size_t repaired = repair_scene_scale_refs(settings, ovi.base_width, ovi.base_height);
		if (repaired > 0) {
			/* scene_load removes every item before re-adding it: keep the sources alive meanwhile (a
			 * source used only by this scene would otherwise be destroyed and its item lost), as the OBS
			 * frontend undo does. */
			std::vector<obs_source_t *> held;
			obs_scene_enum_items(scene, hold_item_source, &held);
			obs_source_load(scene_src);
			for (obs_source_t *s : held)
				obs_source_release(s);
			obs_log(LOG_INFO, "owned canvas: repaired scale_ref of %zu item(s) in scene '%s' (%ux%u)", repaired,
				obs_source_get_name(scene_src), ovi.base_width, ovi.base_height);
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
		 * does not persist it). Only fill an empty channel 0. */
		obs_source_t *current = obs_canvas_get_channel(canvas, 0);
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
		obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
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
	obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
	obs_scene_release(scene); // the canvas holds its own refs (SCENE_REF + channel)
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
		std::lock_guard<std::mutex> lock(m_mutex);
		m_scene_name = name ? name : "";
		m_links = std::move(links);
		return;
	}

	obs_canvas_t *c = get_ref();
	std::string name;
	if (c) {
		obs_source_t *src = obs_canvas_get_channel(c, 0);
		if (src) {
			const char *n = obs_source_get_name(src);
			name = n ? n : "";
			obs_source_release(src);
		}
		obs_canvas_release(c);
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!name.empty())
		m_scene_name = name;
	if (!m_scene_name.empty())
		obs_data_set_string(data, kSceneSaveKey, m_scene_name.c_str());
	scene_link_save(data, m_links);
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

int OwnedCanvas::switch_delay_ms(obs_source_t *transition)
{
	if (!transition)
		return 0;

	const char *id = obs_source_get_id(transition);
	const std::string transition_id = id ? id : "";
	std::int64_t tp_type = 0;
	std::int64_t point = 0;
	if (transition_id == kStingerTransitionId) {
		obs_data_t *settings = obs_source_get_settings(transition);
		if (settings) {
			tp_type = obs_data_get_int(settings, "tp_type");
			point = obs_data_get_int(settings, "transition_point");
			obs_data_release(settings);
		}
	}

	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
		return 0;
	return linked_switch_delay_ms(transition_id, tp_type, point, ovi.fps_num, ovi.fps_den);
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
		if (target) {
			std::string active;
			obs_source_t *current = obs_canvas_get_channel(canvas, 0);
			if (current) {
				const char *n = obs_source_get_name(current);
				active = n ? n : "";
				obs_source_release(current);
			}
			if (active != *target) {
				obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas, target->c_str());
				if (scene) {
					obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
					obs_scene_release(scene);
					obs_log(LOG_INFO, "scene link: \"%s\" -> \"%s\"", main_name.c_str(),
						target->c_str());
				}
			}
		}
	}
	obs_canvas_release(canvas);
}

} // namespace smulti
