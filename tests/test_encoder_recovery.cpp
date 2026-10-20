// Runtime encoder failure recovery: pure counting / fallback rule per endpoint session. No OBS.
#include "TestRunner.hpp"
#include "core/EncoderRecovery.hpp"
#include <string>

using smulti::EncoderBackend;
using smulti::EncoderRecoveryState;

SMULTI_TEST(encoder_recovery_counts_only_encode_errors)
{
	EncoderRecoveryState s;
	/* network codes never count and never mark the reconnect */
	for (int code : {smulti::kStopCodeConnectFailed, smulti::kStopCodeError, smulti::kStopCodeDisconnected}) {
		EXPECT_TRUE(!smulti::on_stop_code(s, code, EncoderBackend::NVENC));
		EXPECT_EQ(s.consecutive_encoder_failures, 0);
		EXPECT_TRUE(!s.after_encode_error);
	}
	/* a create/start failure in a reconnect after a network drop does not count either */
	EXPECT_TRUE(!smulti::on_restart_failure(s, EncoderBackend::NVENC));
	EXPECT_EQ(s.consecutive_encoder_failures, 0);

	/* an encode error counts and marks the following reconnect */
	EXPECT_TRUE(!smulti::on_stop_code(s, smulti::kStopCodeEncodeError, EncoderBackend::NVENC));
	EXPECT_EQ(s.consecutive_encoder_failures, 1);
	EXPECT_TRUE(s.after_encode_error);
	/* a network drop afterwards clears the mark, the count stays */
	smulti::on_stop_code(s, smulti::kStopCodeDisconnected, EncoderBackend::NVENC);
	EXPECT_EQ(s.consecutive_encoder_failures, 1);
	EXPECT_TRUE(!s.after_encode_error);
	EXPECT_TRUE(!smulti::on_restart_failure(s, EncoderBackend::NVENC));
	EXPECT_EQ(s.consecutive_encoder_failures, 1);
	EXPECT_TRUE(!s.fallback_active);
}

SMULTI_TEST(encoder_recovery_falls_back_after_two_failed_restarts)
{
	/* encode error, then two failed restarts (create failure, start failure) */
	EncoderRecoveryState a;
	EXPECT_TRUE(!smulti::on_stop_code(a, smulti::kStopCodeEncodeError, EncoderBackend::NVENC));
	EXPECT_TRUE(!smulti::on_restart_failure(a, EncoderBackend::NVENC));
	EXPECT_TRUE(!a.fallback_active);
	EXPECT_EQ(smulti::effective_backend(EncoderBackend::NVENC, a, true), EncoderBackend::NVENC);
	EXPECT_TRUE(smulti::on_restart_failure(a, EncoderBackend::NVENC)); // switched on by this call
	EXPECT_EQ(a.consecutive_encoder_failures, 3);
	EXPECT_TRUE(a.fallback_active);
	EXPECT_EQ(smulti::effective_backend(EncoderBackend::NVENC, a, true), EncoderBackend::X264);
	/* reported once: a further failure does not switch it on again */
	EXPECT_TRUE(!smulti::on_restart_failure(a, EncoderBackend::NVENC));
	EXPECT_TRUE(a.fallback_active);

	/* a restart that goes live and hits the encode error again is a failed restart too */
	EncoderRecoveryState b;
	smulti::on_stop_code(b, smulti::kStopCodeEncodeError, EncoderBackend::QSV);
	smulti::on_live_for(b, 5000);
	smulti::on_stop_code(b, smulti::kStopCodeEncodeError, EncoderBackend::QSV);
	EXPECT_TRUE(!b.fallback_active);
	smulti::on_live_for(b, smulti::kEncoderStableLiveMs - 1);
	EXPECT_TRUE(smulti::on_stop_code(b, smulti::kStopCodeEncodeError, EncoderBackend::QSV));
	EXPECT_EQ(smulti::effective_backend(EncoderBackend::QSV, b, true), EncoderBackend::X264);
}

SMULTI_TEST(encoder_recovery_x264_never_falls_back)
{
	EncoderRecoveryState s;
	for (int i = 0; i < 6; ++i) {
		EXPECT_TRUE(!smulti::on_stop_code(s, smulti::kStopCodeEncodeError, EncoderBackend::X264));
		EXPECT_TRUE(!smulti::on_restart_failure(s, EncoderBackend::X264));
	}
	EXPECT_TRUE(!s.fallback_active);
	EXPECT_EQ(smulti::effective_backend(EncoderBackend::X264, s, true), EncoderBackend::X264);
}

SMULTI_TEST(encoder_recovery_fallback_is_session_only)
{
	EncoderRecoveryState session;
	smulti::on_stop_code(session, smulti::kStopCodeEncodeError, EncoderBackend::NVENC);
	smulti::on_restart_failure(session, EncoderBackend::NVENC);
	smulti::on_restart_failure(session, EncoderBackend::NVENC);
	EXPECT_EQ(smulti::effective_backend(EncoderBackend::NVENC, session, true), EncoderBackend::X264);
	/* the configured backend is an input only: the next start begins with a fresh state */
	const EncoderRecoveryState next_start{};
	EXPECT_EQ(smulti::effective_backend(EncoderBackend::NVENC, next_start, true), EncoderBackend::NVENC);
	EXPECT_EQ(next_start.consecutive_encoder_failures, 0);
	EXPECT_TRUE(!next_start.fallback_active);
}

SMULTI_TEST(encoder_recovery_stable_live_resets_counter)
{
	EncoderRecoveryState s;
	smulti::on_stop_code(s, smulti::kStopCodeEncodeError, EncoderBackend::NVENC);
	smulti::on_restart_failure(s, EncoderBackend::NVENC);
	EXPECT_EQ(s.consecutive_encoder_failures, 2);
	smulti::on_live_for(s, smulti::kEncoderStableLiveMs - 1); // not long enough
	EXPECT_EQ(s.consecutive_encoder_failures, 2);
	smulti::on_live_for(s, smulti::kEncoderStableLiveMs);
	EXPECT_EQ(s.consecutive_encoder_failures, 0);
	EXPECT_TRUE(!s.after_encode_error);
	/* two more failures after the reset are not enough */
	smulti::on_stop_code(s, smulti::kStopCodeEncodeError, EncoderBackend::NVENC);
	EXPECT_TRUE(!smulti::on_restart_failure(s, EncoderBackend::NVENC));
	EXPECT_TRUE(!s.fallback_active);

	/* an active fallback stays for the session after a stable minute */
	EncoderRecoveryState f;
	smulti::on_stop_code(f, smulti::kStopCodeEncodeError, EncoderBackend::NVENC);
	smulti::on_restart_failure(f, EncoderBackend::NVENC);
	smulti::on_restart_failure(f, EncoderBackend::NVENC);
	smulti::on_live_stable(f);
	EXPECT_EQ(f.consecutive_encoder_failures, 0);
	EXPECT_TRUE(f.fallback_active);
	EXPECT_EQ(smulti::effective_backend(EncoderBackend::NVENC, f, true), EncoderBackend::X264);
}

SMULTI_TEST(encoder_recovery_no_x264_available_keeps_backend)
{
	EncoderRecoveryState s;
	smulti::on_stop_code(s, smulti::kStopCodeEncodeError, EncoderBackend::NVENC);
	smulti::on_restart_failure(s, EncoderBackend::NVENC);
	smulti::on_restart_failure(s, EncoderBackend::NVENC);
	EXPECT_TRUE(s.fallback_active);
	EXPECT_EQ(smulti::effective_backend(EncoderBackend::NVENC, s, false), EncoderBackend::NVENC);
	EXPECT_EQ(smulti::effective_backend(EncoderBackend::AMF, s, false), EncoderBackend::AMF);
}

/* ---- first start of a session (TASK-42) ---- */

SMULTI_TEST(encoder_recovery_first_start_refusal_falls_back)
{
	using smulti::StartFailure;
	using smulti::StartFailureKind;
	const StartFailure create{StartFailureKind::EncoderCreate};
	StartFailure refused{StartFailureKind::StartRefused};
	refused.video_encoder_error = true; // e.g. NVENC "too many sessions"
	StartFailure refused_silent{StartFailureKind::StartRefused};
	refused_silent.service_ready = true; // no encoder text, but the service could connect: only the encoder is left
	StartFailure encode_stop{StartFailureKind::StopBeforeLive};
	encode_stop.stop_code = smulti::kStopCodeEncodeError;

	for (const StartFailure &f : {create, refused, refused_silent, encode_stop}) {
		EXPECT_TRUE(smulti::is_encoder_start_failure(f));
		EncoderRecoveryState s;
		/* one immediate retry with x264: no 3-failure count at the first start */
		EXPECT_TRUE(smulti::on_first_start_failure(s, f, EncoderBackend::NVENC, true));
		EXPECT_TRUE(s.fallback_active);
		EXPECT_EQ(smulti::effective_backend(EncoderBackend::NVENC, s, true), EncoderBackend::X264);
		/* the x264 retry fails too: no second retry */
		EXPECT_TRUE(!smulti::on_first_start_failure(s, f, EncoderBackend::NVENC, true));
		/* session only: the next start() resets the state, the configured backend is used again */
		s = EncoderRecoveryState{};
		EXPECT_EQ(smulti::effective_backend(EncoderBackend::NVENC, s, true), EncoderBackend::NVENC);
	}

	/* an x264 endpoint or a build without x264 has nothing to fall back to */
	EncoderRecoveryState x;
	EXPECT_TRUE(!smulti::on_first_start_failure(x, create, EncoderBackend::X264, true));
	EXPECT_TRUE(!x.fallback_active);
	EXPECT_TRUE(!smulti::on_first_start_failure(x, create, EncoderBackend::NVENC, false));
	EXPECT_TRUE(!x.fallback_active);
}

SMULTI_TEST(encoder_recovery_first_start_network_error_no_fallback)
{
	using smulti::StartFailure;
	using smulti::StartFailureKind;
	/* connect failed, generic error, disconnected, invalid key, bad path: never the encoder's */
	for (int code : {smulti::kStopCodeConnectFailed, smulti::kStopCodeError, smulti::kStopCodeDisconnected, -3, -1}) {
		StartFailure f{StartFailureKind::StopBeforeLive};
		f.stop_code = code;
		EXPECT_TRUE(!smulti::is_encoder_start_failure(f));
		EncoderRecoveryState s;
		EXPECT_TRUE(!smulti::on_first_start_failure(s, f, EncoderBackend::NVENC, true));
		EXPECT_TRUE(!s.fallback_active);
		EXPECT_EQ(smulti::effective_backend(EncoderBackend::NVENC, s, true), EncoderBackend::NVENC);
	}
	/* refused before the encoder step (the service could not try to connect, no encoder error) */
	StartFailure service{StartFailureKind::StartRefused};
	EXPECT_TRUE(!smulti::is_encoder_start_failure(service));
	EncoderRecoveryState s;
	EXPECT_TRUE(!smulti::on_first_start_failure(s, service, EncoderBackend::NVENC, true));
	EXPECT_TRUE(!s.fallback_active);
}

SMULTI_TEST(encoder_recovery_start_refusal_names_the_precondition)
{
	using smulti::StartFailure;
	using smulti::StartFailureKind;
	/* libobs refuses obs_output_start silently (LOG_DEBUG only) when output->service is NULL,
	 * e.g. after the service was released right after obs_output_set_service (no addref there). */
	StartFailure detached{StartFailureKind::StartRefused};
	detached.service_attached = false;
	EXPECT_EQ(std::string(smulti::start_refusal_reason(detached)), std::string("no streaming service attached to the output"));
	EXPECT_TRUE(!smulti::is_encoder_start_failure(detached));

	StartFailure active{StartFailureKind::StartRefused};
	active.output_active = true;
	EXPECT_EQ(std::string(smulti::start_refusal_reason(active)), std::string("the output is still active"));

	StartFailure no_server{StartFailureKind::StartRefused};
	EXPECT_EQ(std::string(smulti::start_refusal_reason(no_server)), std::string("the service cannot try to connect (empty server URL)"));

	StartFailure venc{StartFailureKind::StartRefused};
	venc.service_ready       = true;
	venc.video_encoder_error = true;
	EXPECT_EQ(std::string(smulti::start_refusal_reason(venc)), std::string("the video encoder failed to initialize"));

	StartFailure enc{StartFailureKind::StartRefused};
	enc.service_ready = true;
	EXPECT_EQ(std::string(smulti::start_refusal_reason(enc)),
		  std::string("encoder initialization refused (video or audio)"));
}
