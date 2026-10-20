/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "EncoderRecovery.hpp"

namespace smulti {

bool on_encoder_failure(EncoderRecoveryState &state, EncoderBackend configured)
{
	++state.consecutive_encoder_failures;
	if (state.fallback_active || configured == EncoderBackend::X264 ||
	    state.consecutive_encoder_failures < kEncoderFallbackAfterFailures)
		return false;
	state.fallback_active = true;
	return true;
}

bool on_stop_code(EncoderRecoveryState &state, int code, EncoderBackend configured)
{
	if (code != kStopCodeEncodeError) {
		state.after_encode_error = false; // network drop: the next restart failures are not the encoder's
		return false;
	}
	state.after_encode_error = true;
	return on_encoder_failure(state, configured);
}

bool on_restart_failure(EncoderRecoveryState &state, EncoderBackend configured)
{
	return state.after_encode_error && on_encoder_failure(state, configured);
}

void on_live_stable(EncoderRecoveryState &state)
{
	state.consecutive_encoder_failures = 0;
	state.after_encode_error           = false;
}

void on_live_for(EncoderRecoveryState &state, int64_t live_ms)
{
	if (live_ms >= kEncoderStableLiveMs)
		on_live_stable(state);
}

bool is_encoder_start_failure(const StartFailure &f)
{
	switch (f.kind) {
	case StartFailureKind::EncoderCreate:  return true;
	case StartFailureKind::StartRefused:   return f.video_encoder_error || f.service_ready;
	case StartFailureKind::StopBeforeLive: return f.stop_code == kStopCodeEncodeError;
	}
	return false;
}

bool on_first_start_failure(EncoderRecoveryState &state, const StartFailure &f, EncoderBackend configured,
			    bool x264_available)
{
	if (!is_encoder_start_failure(f) || configured == EncoderBackend::X264 || !x264_available ||
	    state.fallback_active)
		return false;
	state.fallback_active = true;
	return true;
}

EncoderBackend effective_backend(EncoderBackend configured, const EncoderRecoveryState &state, bool x264_available)
{
	return state.fallback_active && x264_available ? EncoderBackend::X264 : configured;
}

} // namespace smulti
