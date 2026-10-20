/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "LockPolicy.hpp"

#include <algorithm>

namespace smulti::lock_policy {

namespace {
const preview_edit::Item *find(const std::vector<preview_edit::Item> &items, int64_t id)
{
	for (const auto &it : items)
		if (it.id == id)
			return &it;
	return nullptr;
}
} // namespace

std::vector<int64_t> editable_ids(const std::vector<preview_edit::Item> &items, const std::vector<int64_t> &selection)
{
	std::vector<int64_t> out;
	for (int64_t id : selection) {
		const preview_edit::Item *it = find(items, id);
		if (it && may_write(it->locked) && std::find(out.begin(), out.end(), id) == out.end())
			out.push_back(id);
	}
	return out;
}

bool can_edit(const std::vector<preview_edit::Item> &items, int64_t id)
{
	const preview_edit::Item *it = find(items, id);
	return it && may_write(it->locked);
}

std::vector<int64_t> lock_all_changes(const std::vector<preview_edit::Item> &items, bool lock)
{
	std::vector<int64_t> out;
	for (const auto &it : items)
		if (it.locked != lock)
			out.push_back(it.id);
	return out;
}

Pick pick_item(const std::vector<preview_edit::Item> &bottom_to_top, preview_edit::Vec2 canvas_pt)
{
	if (const int64_t id = preview_edit::hit_item(bottom_to_top, canvas_pt))
		return {id, true};
	return {preview_edit::hit_item_selectable(bottom_to_top, canvas_pt), false};
}

std::vector<preview_edit::Item> plan_nudge(const std::vector<preview_edit::Item> &items,
					   const std::vector<int64_t> &selection, int dx, int dy, bool shift)
{
	std::vector<preview_edit::Item> out;
	for (int64_t id : editable_ids(items, selection)) {
		preview_edit::Item next = *find(items, id);
		next.pos = preview_edit::nudge(next.pos, dx, dy, shift);
		out.push_back(next);
	}
	return out;
}

} // namespace smulti::lock_policy
