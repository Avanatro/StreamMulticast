/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "OutputState.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct obs_data;
typedef struct obs_data obs_data_t;

namespace smulti {

/**
 * Pure live-health alert rules (OBS-free, Qt-free, no clock inside). The adapter
 * (pipeline/AlertMonitor) feeds one AlertInput per endpoint per 500 ms tick with explicit
 * millisecond times from a monotonic clock and acts on the returned transitions.
 */

/** Alert reason strings (also the "reason" field of the websocket event). */
inline constexpr const char *kAlertReasonReconnecting = "reconnecting";
inline constexpr const char *kAlertReasonFailed       = "failed";
inline constexpr const char *kAlertReasonDropped      = "dropped_frames";
inline constexpr const char *kAlertReasonLowBitrate   = "low_bitrate";

enum class AlertLevel : int { None = 0, Warning = 1, Critical = 2 };

/** "warning" | "critical" | "none". */
const char *alert_level_name(AlertLevel level);
/** "idle" | "starting" | "live" | "reconnecting" | "failed". */
const char *alert_state_name(OutputState state);

struct AlertThresholds {
	int64_t drop_window_ms        = 10000;  ///< window of the drop ratio
	double  drop_ratio            = 0.02;   ///< dropped_delta / frames_delta at or above this ...
	int64_t drop_min_frames       = 10;     ///< ... and at least this many dropped frames in the window
	double  low_bitrate_fraction  = 0.5;    ///< actual < fraction * target ...
	int64_t low_bitrate_hold_ms   = 15000;  ///< ... continuously for this long
	int64_t startup_grace_ms      = 10000;  ///< drop / bitrate checks start this long after Live began
	int64_t clear_hold_ms         = 10000;  ///< a raised alert clears after its condition was false this long
};

/** One endpoint at one tick. Counters are cumulative per session (HealthSnapshot). */
struct AlertInput {
	std::string endpoint_id;
	std::string endpoint_name;
	OutputState state          = OutputState::Idle;
	int         target_kbps    = 0;
	double      actual_kbps    = 0.0;
	uint64_t    dropped_total  = 0;
	uint64_t    frames_total   = 0;
	int64_t     now_ms         = 0;
};

/** Edge: an alert reason was raised or cleared at this tick. Carries what the event payload needs. */
struct AlertTransition {
	bool        raised = false;           ///< false = cleared
	AlertLevel  level  = AlertLevel::None; ///< level of the reason that changed
	std::string reason;                    ///< kAlertReason*
	std::string endpoint_id;
	std::string endpoint_name;
	OutputState state  = OutputState::Idle;
};

/** Current alert picture of one endpoint. */
struct AlertStatus {
	AlertLevel               level = AlertLevel::None; ///< highest level of the active reasons
	std::vector<std::string> reasons;                  ///< active reasons, fixed order
};

class AlertPolicy {
public:
	explicit AlertPolicy(const AlertThresholds &thresholds = AlertThresholds());

	/** Feed one endpoint at one tick; returns this tick's edges (raise/clear), usually empty. */
	std::vector<AlertTransition> update(const AlertInput &in);

	/** Endpoints not in `keep` are dropped; their active alerts are returned as cleared edges. */
	std::vector<AlertTransition> retain_only(const std::vector<std::string> &keep);

	AlertStatus status(const std::string &endpoint_id) const;
	std::map<std::string, AlertStatus> statuses() const;
	AlertLevel max_level() const;

private:
	struct Reason {
		bool    active      = false;
		int64_t false_since = -1;  ///< -1 = condition currently true (or not yet raised)
	};
	struct Sample {
		int64_t  t;
		uint64_t dropped;
		uint64_t frames;
	};
	struct Endpoint {
		std::string name;
		OutputState state       = OutputState::Idle;
		int64_t     live_since  = -1;
		int64_t     low_since   = -1;
		std::vector<Sample> window;
		Reason      reasons[4];  ///< reconnecting, failed, dropped, low bitrate
	};

	void raise(Endpoint &ep, const std::string &id, int idx, std::vector<AlertTransition> &out) const;
	void clear(Endpoint &ep, const std::string &id, int idx, std::vector<AlertTransition> &out) const;
	void hold_clear(Endpoint &ep, const std::string &id, int idx, bool condition, int64_t now,
			std::vector<AlertTransition> &out) const;

	AlertThresholds m_t;
	std::map<std::string, Endpoint> m_endpoints;
};

/**
 * Event payload for a transition: fields endpoint_id, endpoint_name, level, reason, active, state.
 * Never the server URL, the key, a token or last_error (those are not even inputs). Caller releases.
 */
obs_data_t *alert_event_payload(const AlertTransition &transition);

/* ---- user settings (plugin_config/streammulticast/alerts.json) ---- */

inline constexpr const char *kAlertSettingsFile       = "alerts.json";
inline constexpr const char *kAlertSoundKey           = "sound_enabled";
inline constexpr const char *kAlertWebsocketKey       = "websocket_enabled";

struct AlertSettings {
	bool sound_enabled     = false;
	bool websocket_enabled = true;
};

void alert_settings_save(obs_data_t *data, const AlertSettings &settings);
/** Missing keys keep the defaults. */
AlertSettings alert_settings_load(obs_data_t *data);

/* ---- sound throttle ---- */

inline constexpr int64_t kAlertSoundMinGapMs = 30000;

/** True when a sound may play at `now_ms`; records it. At most one per kAlertSoundMinGapMs across endpoints. */
bool alert_sound_allowed(int64_t now_ms, int64_t &last_played_ms);

} // namespace smulti
