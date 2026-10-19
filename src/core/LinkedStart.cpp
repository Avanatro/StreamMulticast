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

} // namespace smulti
