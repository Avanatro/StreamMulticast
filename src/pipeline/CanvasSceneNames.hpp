/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <string>
#include <vector>

struct obs_canvas;
typedef struct obs_canvas obs_canvas_t;

namespace smulti {

/** Scene names of a canvas in libobs enumeration order. Takes and releases every ref itself. */
std::vector<std::string> canvas_scene_names(obs_canvas_t *canvas);

/** True when any source of the canvas (scene or not: they share one name table) uses the name. */
bool canvas_has_name(obs_canvas_t *canvas, const std::string &name);

/**
 * `base`, else `base<open>2<close>`, `base<open>3<close>` ... the first name free in the canvas.
 * Scene names: open " ", close "". Import copies: open " (", close ")".
 */
std::string unique_name_in(obs_canvas_t *canvas, const std::string &base, const char *open = " ",
			   const char *close = "");

} // namespace smulti
