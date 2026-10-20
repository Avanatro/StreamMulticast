/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "PreviewEdit.hpp"

#include <cstdint>
#include <vector>

/**
 * Pure lock rules for the vertical editor (OBS-free, Qt-free). The lock flag itself lives in libobs
 * (obs_sceneitem_locked); callers pass a snapshot of it in preview_edit::Item::locked. Nothing here stores
 * lock state.
 */
namespace smulti::lock_policy {

/**
 * The selected ids a drag, resize or nudge may change: the ones that exist in `items`, are unlocked, listed
 * once, in selection order. Locked members stay put, unlocked members move; unknown ids are ignored.
 */
std::vector<int64_t> editable_ids(const std::vector<preview_edit::Item> &items, const std::vector<int64_t> &selection);

/** True when the item exists in `items` and is not locked. */
bool can_edit(const std::vector<preview_edit::Item> &items, int64_t id);

/** Ids whose flag must change so that every item is locked (lock = true) or unlocked; scene order kept. */
std::vector<int64_t> lock_all_changes(const std::vector<preview_edit::Item> &items, bool lock);

} // namespace smulti::lock_policy
