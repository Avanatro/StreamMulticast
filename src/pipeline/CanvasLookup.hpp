/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/CanvasResolve.hpp"

#include <string>
#include <vector>

// libobs forward declaration
struct obs_canvas;
typedef struct obs_canvas obs_canvas_t;

namespace smulti {

/**
 * Snapshot every canvas libobs knows about (obs_enum_canvases).
 *
 * Safe to call from any thread, including an endpoint's reconnect thread:
 * obs_enum_canvases takes libobs's own canvases mutex.  The frontend canvas
 * API is deliberately not used here because it is UI-thread only.
 */
std::vector<CanvasInfo> snapshot_canvases();

/**
 * Strong reference to the canvas with this uuid, or nullptr when it is gone.
 * The caller releases it with obs_canvas_release().
 */
obs_canvas_t *acquire_canvas(const std::string &uuid);

} // namespace smulti
