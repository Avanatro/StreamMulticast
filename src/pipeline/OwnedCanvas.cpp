/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "OwnedCanvas.hpp"

#include "CanvasLookup.hpp"
#include "OutputController.hpp"
#include "../core/EndpointRegistry.hpp"
#include "../core/OwnedCanvasPolicy.hpp"
#include "../plugin-support.h"

#include <obs.h>
#include <obs-frontend-api.h>

#include <utility>
#include <vector>

namespace smulti {

namespace {

constexpr const char *kDefaultSceneName = "V Main";

bool count_scene(void *param, obs_source_t *)
{
	++*static_cast<size_t *>(param);
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

} // anonymous namespace

OwnedCanvas::~OwnedCanvas()
{
	release();
}

void OwnedCanvas::remember(obs_canvas_t *canvas)
{
	obs_weak_canvas_t *weak = obs_canvas_get_weak_canvas(canvas);
	const char *uuid = obs_canvas_get_uuid(canvas);

	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_weak)
		obs_weak_canvas_release(m_weak);
	m_weak = weak;
	m_uuid = uuid ? uuid : "";
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
	if (obs_canvas_has_video(canvas))
		ensure_default_scene(canvas);
	obs_canvas_release(canvas);
}

void OwnedCanvas::ensure_default_scene(obs_canvas_t *canvas)
{
	size_t scenes = 0;
	obs_canvas_enum_scenes(canvas, count_scene, &scenes);
	if (scenes > 0)
		return;

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
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_weak) {
		obs_weak_canvas_release(m_weak);
		m_weak = nullptr;
	}
	m_uuid.clear();
}

obs_canvas_t *OwnedCanvas::get_ref() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_weak ? obs_weak_canvas_get_canvas(m_weak) : nullptr;
}

std::string OwnedCanvas::uuid() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_uuid;
}

} // namespace smulti
