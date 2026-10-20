/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "AutosavePolicy.hpp"

#include <algorithm>

namespace smulti {

AutosavePolicy::AutosavePolicy(int64_t delay_ms, int64_t max_wait_ms) : m_delay(delay_ms), m_max_wait(max_wait_ms) {}

void AutosavePolicy::request(int64_t now_ms)
{
	if (m_blocked)
		return;
	if (!m_pending) {
		m_pending = true;
		m_first = now_ms;
		m_deadline = now_ms + m_delay;
		return;
	}
	m_deadline = std::min(now_ms + m_delay, m_first + m_max_wait);
}

bool AutosavePolicy::due(int64_t now_ms)
{
	if (!m_pending || now_ms < m_deadline)
		return false;
	m_pending = false;
	return true;
}

void AutosavePolicy::set_blocked(bool blocked)
{
	m_blocked = blocked;
	if (blocked)
		m_pending = false;
}

} // namespace smulti
