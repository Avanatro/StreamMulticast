/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/Endpoint.hpp"
#include "../core/VerticalRecord.hpp"

#include <cstddef>
#include <set>
#include <vector>
#include <string>

// libobs types forward-declared to avoid pulling in obs.h in every translation unit
struct obs_encoder;
typedef struct obs_encoder obs_encoder_t;
struct video_output;
typedef struct video_output video_t;

namespace smulti {

/**
 * EncoderFactory — creates and releases libobs encoder instances.
 *
 * One video encoder + one audio encoder are created per active Endpoint.
 * The factory is stateless — it only creates; lifetimes are owned by
 * OutputController which releases encoders when the output stops.
 *
 * Video encoder IDs used:
 *   x264  → "obs_x264"
 *   nvenc → "jim_nvenc"  (fallback: "ffmpeg_nvenc")
 *   qsv   → "obs_qsv11_v2"
 *   amf   → "h264_texture_amf"
 *
 * Audio encoder ID: "ffmpeg_aac" (always available via FFmpeg)
 */
class EncoderFactory {
public:
	EncoderFactory() = default;
	~EncoderFactory() = default;

	/**
	 * Create a video encoder for the given endpoint settings.
	 *
	 * `video` is the video pipeline the encoder reads frames from: the main
	 * one (obs_get_video()) or a canvas's own (obs_canvas_get_video()).  It
	 * must be non-null (null logs an error and returns nullptr), and the
	 * caller must keep the owning canvas alive for as long as the encoder
	 * is in use.
	 *
	 * Returns nullptr if the requested encoder backend is unavailable
	 * (e.g., NVENC on a machine without NVIDIA GPU) or if creation fails.
	 *
	 * obs_encoder_t* lifetime: the caller (OutputController) must call
	 * obs_encoder_release() when done.  Releasing after the output has
	 * stopped is safe regardless of order relative to obs_output_stop(),
	 * because obs_output_set_video/audio_encoder() takes its OWN reference
	 * via obs_encoder_get_ref() (see libobs obs-output.c) — the output and
	 * OutputController each hold an independent ref and release their own.
	 */
	obs_encoder_t *create_video_encoder(const Endpoint &ep,
	                                    const std::string &name_hint,
	                                    video_t *video) const;

	/**
	 * Same, with `backend` instead of ep.encoder_backend (runtime encoder fallback of one session,
	 * see core/EncoderRecovery); every other setting comes from `ep`, which is never changed.
	 */
	obs_encoder_t *create_video_encoder(const Endpoint &ep,
	                                    const std::string &name_hint,
	                                    video_t *video,
	                                    EncoderBackend backend) const;

	/**
	 * Create an AAC audio encoder for the given endpoint.
	 *
	 * AVANATRO-VERIFY: obs_audio_encoder_create — the "mixer_idx" parameter
	 * (0 = main OBS audio mix).  Confirm this grabs the same audio as
	 * OBS's main output (it should — mixer 0 is always the default mix).
	 */
	obs_encoder_t *create_audio_encoder(const Endpoint &ep,
	                                    const std::string &name_hint) const;

	/**
	 * AAC encoder on the main audio, mix `mixer_idx` (0 = track 1). Used by
	 * create_audio_encoder and by the vertical recorder. Caller releases.
	 */
	obs_encoder_t *create_aac_encoder(const std::string &name, int bitrate_kbps, size_t mixer_idx) const;

	/**
	 * Quality-based video encoder for a recording (choose_vertical_record_encoder),
	 * attached to `video` (a canvas's own video_t). Same lifetime rule as
	 * create_video_encoder: the caller keeps the owning canvas alive while the
	 * encoder is in use and releases the encoder. Null video or a failed
	 * create logs and returns nullptr.
	 */
	obs_encoder_t *create_record_video_encoder(const RecordEncoderChoice &choice,
	                                           const std::string &name,
	                                           video_t *video) const;

	/** Registered encoder type ids (enumerated once per process). */
	static const std::set<std::string> &available_encoder_types();

	/**
	 * Query which encoder backends are available on this machine.
	 * Used by EndpointDialog to populate the backend dropdown.
	 *
	 * Always includes X264 (software x264 is guaranteed available in OBS).
	 * Includes NVENC/QSV/AMF only if obs_enum_encoder_types reports them.
	 *
	 * obs_enum_encoder_types uses indexed iteration in OBS 31.x:
	 *   EXPORT bool obs_enum_encoder_types(size_t idx, const char **id);
	 * (there is no callback-based overload — see is_encoder_available()).
	 */
	static std::vector<EncoderBackend> available_backends();

	/**
	 * Human-readable label for a backend (for UI display).
	 */
	static std::string backend_label(EncoderBackend backend);

	/**
	 * OBS encoder type ID string for a backend.
	 * Returns the preferred ID; caller should check availability first.
	 */
	static std::string encoder_type_id(EncoderBackend backend);

private:
	/**
	 * Check whether a given encoder type ID is registered in OBS.
	 */
	static bool is_encoder_available(const std::string &type_id);
};

} // namespace smulti
