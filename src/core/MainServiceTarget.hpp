/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "EndpointValidation.hpp"

#include <optional>

namespace smulti {

/**
 * Read the server URL and stream key of the OBS main streaming service.
 * Plugin-only (needs the OBS frontend); not covered by the unit tests.
 * Returns std::nullopt when no service is configured or it exposes no URL.
 * Must be called on the OBS UI thread.
 */
std::optional<StreamTarget> read_main_service_target();

} // namespace smulti
