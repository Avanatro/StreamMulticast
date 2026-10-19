/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "LinkedStart.hpp"

namespace smulti {

std::vector<Endpoint> select_linked_for_start(const std::vector<Endpoint> &all)
{
	std::vector<Endpoint> out;
	for (const auto &ep : all) {
		if (ep.enabled && ep.linked_to_main)
			out.push_back(ep);
	}
	return out;
}

LinkedStartPlan plan_linked_start(const std::vector<Endpoint> &all, const std::optional<StreamTarget> &main_target)
{
	LinkedStartPlan plan;
	for (const auto &ep : select_linked_for_start(all)) {
		/* Compare against the endpoints already accepted (first one wins) and the main service. */
		DuplicateResult dup = find_duplicate(ep, plan.to_start, main_target);
		if (dup.kind != DuplicateKind::None)
			plan.skipped_duplicates.push_back({ep.name, dup.kind});
		else
			plan.to_start.push_back(ep);
	}
	return plan;
}

} // namespace smulti
