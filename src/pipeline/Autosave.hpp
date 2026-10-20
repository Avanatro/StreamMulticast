/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

namespace smulti {

/**
 * Debounced save of the OBS scene collection after the plugin edited the owned vertical canvas.
 * Plugin edits (libobs calls) never reach OBSBasic::SaveProject on their own, so a crash lost them.
 * The timing rule is core/AutosavePolicy; this adapter owns the one QTimer and the only
 * obs_frontend_save() call. UI thread only.
 */

/** Create the timer (UI thread). Starts blocked until autosave_set_blocked(false). Idempotent. */
void autosave_start();

/** Destroy the timer (UI thread). Later requests are dropped. Idempotent. */
void autosave_stop();

/** Block (collection load, teardown) or unblock. Blocking stops the timer and drops a pending save. */
void autosave_set_blocked(bool blocked);

/** The only entry point for edit sites: one call per finished edit, UI thread only. `reason` is for the log. */
void request_autosave(const char *reason);

} // namespace smulti
