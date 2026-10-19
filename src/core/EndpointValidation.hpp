/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "Endpoint.hpp"

#include <optional>
#include <string>
#include <vector>

namespace smulti {

/** Where a stream goes: ingest URL plus stream key. */
struct StreamTarget {
	std::string server_url;
	std::string stream_key;
};

/**
 * Normalize an ingest URL for comparison only (never stored):
 * trim whitespace, lowercase scheme and host (path stays case-sensitive),
 * strip trailing '/'. Returns "" for blank input.
 */
std::string normalize_server_url(const std::string &url);

/**
 * Two targets are duplicates when the normalized server URLs are equal AND
 * the (trimmed) stream keys are equal and non-empty.
 */
bool is_duplicate_target(const StreamTarget &a, const StreamTarget &b);

enum class DuplicateKind {
	None,
	OtherEndpoint, ///< Same target as another enabled endpoint
	MainService,   ///< Same target as the OBS main streaming service
};

struct DuplicateResult {
	DuplicateKind kind = DuplicateKind::None;
	std::string other_name; ///< Name of the other endpoint (OtherEndpoint only). Never the key.
};

/**
 * Check `candidate` against the other enabled endpoints (the endpoint with the
 * same id is ignored) and, when given, the OBS main service target.
 * Other endpoints win over the main service when both match.
 */
DuplicateResult find_duplicate(const Endpoint &candidate, const std::vector<Endpoint> &all,
			       const std::optional<StreamTarget> &main_target);

} // namespace smulti
