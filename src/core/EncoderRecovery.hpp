/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "Endpoint.hpp"

#include <cstdint>

namespace smulti {

/**
 * Pure runtime encoder-failure recovery rule for one endpoint session (OBS-free, no clock inside).
 * pipeline/OutputController holds one EncoderRecoveryState per session under its mutex and feeds it.
 *
 * An encoder failure is a stop with OBS_OUTPUT_ENCODE_ERROR, or an encoder-create / obs_output_start
 * failure inside a reconnect that follows an encode error. After the first encode error plus two failed
 * restarts (consecutive_encoder_failures >= 3) a non-x264 endpoint falls back to x264 for the rest of the
 * session: the state lives only in the controller and is reset by the next start(), the endpoint config
 * is never changed. Live for 60 s without a new encoder failure resets the counter (the fallback stays).
 * Network stop codes never count.
 */

/** libobs stop codes (obs-defs.h, checked by a static_assert in OutputController.cpp). */
inline constexpr int kStopCodeConnectFailed = -2; ///< OBS_OUTPUT_CONNECT_FAILED
inline constexpr int kStopCodeError         = -4; ///< OBS_OUTPUT_ERROR
inline constexpr int kStopCodeDisconnected  = -5; ///< OBS_OUTPUT_DISCONNECTED
inline constexpr int kStopCodeEncodeError   = -8; ///< OBS_OUTPUT_ENCODE_ERROR

/** Failures in a row (the first encode error plus two failed restarts) that switch the fallback on. */
inline constexpr int kEncoderFallbackAfterFailures = 3;
/** Continuous Live time that resets the failure counter. */
inline constexpr int64_t kEncoderStableLiveMs = 60000;

struct EncoderRecoveryState {
	int  consecutive_encoder_failures = 0;
	bool fallback_active              = false;
	bool after_encode_error           = false; ///< the running reconnect follows an encode error
};

/** Counts one encoder failure. True when this call switched the fallback on (configured != X264). */
bool on_encoder_failure(EncoderRecoveryState &state, EncoderBackend configured);

/**
 * Feeds a reconnect-eligible stop code: OBS_OUTPUT_ENCODE_ERROR counts (on_encoder_failure) and marks
 * the following reconnect; every other code counts nothing and clears that mark. True when the
 * fallback was switched on.
 */
bool on_stop_code(EncoderRecoveryState &state, int code, EncoderBackend configured);

/**
 * Encoder-create or obs_output_start failure inside a reconnect: counts only when that reconnect
 * follows an encode error. True when the fallback was switched on.
 */
bool on_restart_failure(EncoderRecoveryState &state, EncoderBackend configured);

/** Live for kEncoderStableLiveMs without a new encoder failure: counter and mark reset, fallback stays. */
void on_live_stable(EncoderRecoveryState &state);

/** Calls on_live_stable when the session was Live for at least kEncoderStableLiveMs. */
void on_live_for(EncoderRecoveryState &state, int64_t live_ms);

/** X264 while the fallback is active and x264 is registered, else `configured`. */
EncoderBackend effective_backend(EncoderBackend configured, const EncoderRecoveryState &state, bool x264_available);

} // namespace smulti
