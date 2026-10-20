/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <cstdint>

namespace smulti {

/** Quiet time after the last edit before the scene collection is saved. */
inline constexpr int64_t kAutosaveDelayMs = 2000;
/** A continuous series of edits still saves at least this long after its first edit. */
inline constexpr int64_t kAutosaveMaxWaitMs = 10000;

/**
 * Pure debounce rule for the scene collection autosave (OBS-free, Qt-free, no clock inside).
 * The adapter (pipeline/Autosave) passes explicit millisecond times from a monotonic clock.
 *
 *   request(now)     an edit finished: arm a deadline now + delay, extended by each further request but
 *                    never later than (first request of the burst) + max wait
 *   due(now)         true exactly once when the deadline has passed, then the burst is over
 *   set_blocked(b)   collection load / teardown: while blocked requests are dropped; blocking cancels a
 *                    pending request, so unblocking never fires a stale save
 */
class AutosavePolicy {
public:
	explicit AutosavePolicy(int64_t delay_ms = kAutosaveDelayMs, int64_t max_wait_ms = kAutosaveMaxWaitMs);

	void request(int64_t now_ms);
	bool due(int64_t now_ms);
	void set_blocked(bool blocked);

	bool blocked() const { return m_blocked; }
	bool pending() const { return m_pending; }
	/** Deadline of the pending save; -1 when nothing is pending. */
	int64_t deadline_ms() const { return m_pending ? m_deadline : -1; }

private:
	int64_t m_delay;
	int64_t m_max_wait;
	bool m_blocked = false;
	bool m_pending = false;
	int64_t m_first = 0;
	int64_t m_deadline = 0;
};

} // namespace smulti
