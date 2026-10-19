/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "Endpoint.hpp"

namespace smulti {

/** Name of the canvas this plugin owns for vertical output. */
inline constexpr const char *kOwnedVerticalCanvasName = "StreamMulticast Vertical";

/**
 * Migrate a removed framing mode (orientation 1 or 2) to a canvas reference:
 * canvas_name becomes kOwnedVerticalCanvasName, canvas_uuid is cleared and the
 * orientation is reset to SourceMatch.
 * Returns true when the endpoint was changed, false when nothing was to do
 * (so a second call on a migrated endpoint returns false).
 */
bool migrate_legacy_orientation(Endpoint &ep);

} // namespace smulti
