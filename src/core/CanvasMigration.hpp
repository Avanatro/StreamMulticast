/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "Endpoint.hpp"

#include <string>
#include <vector>

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

/** One endpoint changed by migrate_config(): name and the framing mode it used (never keys or URLs). */
struct MigratedEndpoint {
	std::string name;
	int old_orientation = 0;
};

struct ConfigMigrationResult {
	std::vector<MigratedEndpoint> migrated;
	/** True when the file on disk is older than the current schema: the caller must write it back. */
	bool needs_save = false;
};

/**
 * Bring endpoints loaded from a config of schema `from_version` up to
 * `to_version`. Pure: no logging, no I/O. needs_save is true whenever
 * from_version < to_version, even if no endpoint changed, so the schema
 * number on disk is bumped as well and the migration runs only once.
 */
ConfigMigrationResult migrate_config(int from_version, int to_version, std::vector<Endpoint> &endpoints);

} // namespace smulti
