/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "Endpoint.hpp"

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

} // namespace smulti
