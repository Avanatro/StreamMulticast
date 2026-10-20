/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

namespace smulti {

/**
 * OutputState — state machine for a single RTMP output.
 *
 * Declared here (pure, no libobs) so core logic such as AlertPolicy can read it without
 * depending on a pipeline header. OutputController owns the transitions.
 *
 * Transitions:
 *   Idle → Starting → Live → Reconnecting → Live
 *                          ↘ FailedHard
 *   Any → Idle (via stop())
 */
enum class OutputState : int {
	Idle         = 0,
	Starting     = 1,
	Live         = 2,
	Reconnecting = 3,
	FailedHard   = 4,
};

} // namespace smulti
