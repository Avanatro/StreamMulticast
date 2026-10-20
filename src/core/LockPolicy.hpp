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

/** The one reason text for a refused write; every mutation path (preview, panel, presets) uses it. */
constexpr const char *kLockedReason = "The item is locked. Unlock it first.";

/** THE gate: whether a mutation (drag, resize, nudge, undo, panel Apply, Fit/Fill, presets) may write an item. */
inline bool may_write(bool locked)
{
	return !locked;
}

/** nullptr when the write may proceed, else kLockedReason. For paths that report a refusal. */
inline const char *write_refusal(bool locked)
{
	return may_write(locked) ? nullptr : kLockedReason;
}

/** What a click at a point hits: the topmost UNLOCKED visible item (editable: may be dragged), else the topmost
 * locked one (selectable only), else id 0. A locked overlay never swallows a click meant for an item below. */
struct Pick {
	int64_t id = 0;
	bool editable = false;
};
Pick pick_item(const std::vector<preview_edit::Item> &bottom_to_top, preview_edit::Vec2 canvas_pt);

/** The arrow-key nudge as a plan: for every editable selected item its moved copy (locked ones are absent). */
std::vector<preview_edit::Item> plan_nudge(const std::vector<preview_edit::Item> &items,
					   const std::vector<int64_t> &selection, int dx, int dy, bool shift);

} // namespace smulti::lock_policy
