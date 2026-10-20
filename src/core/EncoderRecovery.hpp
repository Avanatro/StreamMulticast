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

/**
 * First start of a session (before it was ever Live). Facts the adapter gathers; no OBS types.
 * - EncoderCreate: the video encoder could not be created.
 * - StartRefused: obs_output_start returned false. For rtmp_output that call never touches the network (the
 *   connect runs on its own thread and reports -2/-3 as a stop code): after the service check the only
 *   refusing step is the encoder initialization (libobs rtmp_stream_start / obs_output_initialize_encoders).
 *   So it is an encoder refusal when the video encoder reported an error, or when the service could connect.
 * - StopBeforeLive: a stop code arrived while the session was still Starting; only
 *   OBS_OUTPUT_ENCODE_ERROR is the encoder's, network and key codes never are.
 */
enum class StartFailureKind { EncoderCreate, StartRefused, StopBeforeLive };

struct StartFailure {
	StartFailureKind kind      = StartFailureKind::StartRefused;
	bool video_encoder_error   = false; ///< StartRefused: the video encoder has a last error
	bool service_ready         = false; ///< StartRefused: obs_service_can_try_to_connect was true
	int  stop_code             = 0;     ///< StopBeforeLive
};

/** True when `f` is the encoder's failure (see StartFailure), never for a network or key error. */
bool is_encoder_start_failure(const StartFailure &f);

/**
 * First-start refusal by the encoder: switches the session fallback on at once (no 3-failure count) and
 * returns true, meaning "retry once now with x264". False for a non-encoder failure, an x264 endpoint, no
 * registered x264, or when the fallback is already on (the x264 retry itself failed: no second retry).
 * Session only, like the runtime rule: the endpoint config is never changed.
 */
bool on_first_start_failure(EncoderRecoveryState &state, const StartFailure &f, EncoderBackend configured,
			    bool x264_available);

/** X264 while the fallback is active and x264 is registered, else `configured`. */
EncoderBackend effective_backend(EncoderBackend configured, const EncoderRecoveryState &state, bool x264_available);

} // namespace smulti
