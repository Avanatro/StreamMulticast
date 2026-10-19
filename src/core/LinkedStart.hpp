/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "Endpoint.hpp"
#include "EndpointValidation.hpp"

#include <optional>
#include <string>
#include <vector>

namespace smulti {

/**
 * Pure selection of the endpoints that start together with the OBS main
 * stream. No OBS or Qt dependency, so it is unit-testable.
 *
 * An endpoint is selected only when it is enabled AND linked to the main
 * stream. Order of the input is preserved.
 */
std::vector<Endpoint> select_linked_for_start(const std::vector<Endpoint> &all);

/** An endpoint that was selected for start but skipped because it duplicates a target. */
struct SkippedStart {
	std::string name; ///< Endpoint name (never the stream key)
	DuplicateKind reason = DuplicateKind::None;
};

struct LinkedStartPlan {
	std::vector<Endpoint> to_start;
	std::vector<SkippedStart> skipped_duplicates;
};

/**
 * Like select_linked_for_start, but additionally drops endpoints that would
 * stream to the same server+key as the OBS main service or as an endpoint
 * earlier in the selection (the first one wins).
 */
LinkedStartPlan plan_linked_start(const std::vector<Endpoint> &all, const std::optional<StreamTarget> &main_target);

} // namespace smulti
