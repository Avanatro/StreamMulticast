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

EncoderBackend effective_backend(EncoderBackend configured, const EncoderRecoveryState &state, bool x264_available)
{
	return state.fallback_active && x264_available ? EncoderBackend::X264 : configured;
}

} // namespace smulti
