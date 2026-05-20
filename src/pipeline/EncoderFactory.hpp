/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/Endpoint.hpp"

#include <vector>
#include <string>

// libobs types forward-declared to avoid pulling in obs.h in every translation unit
struct obs_encoder;
typedef struct obs_encoder obs_encoder_t;

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
	 * Returns nullptr if the requested encoder backend is unavailable
	 * (e.g., NVENC on a machine without NVIDIA GPU) or if creation fails.
	 *
	 * AVANATRO-VERIFY: obs_video_encoder_create — confirm the "mixer" index
	 * parameter (3rd positional param in OBS 30+).  In OBS 29 it was
	 * (id, name, settings, hotkey_data).  OBS 30+ added a "mixer" int param
	 * at the end for multi-view outputs.  Check exact signature.
	 *
	 * AVANATRO-VERIFY: obs_encoder_t* lifetime — caller must call
	 * obs_encoder_release() when done.  Confirm whether releasing BEFORE
	 * obs_output_stop() is safe, or whether the output holds a ref.
	 * Correct pattern: stop output → then release encoders.
	 */
	obs_encoder_t *create_video_encoder(const Endpoint &ep,
	                                    const std::string &name_hint) const;

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
	 * Query which encoder backends are available on this machine.
	 * Used by EndpointDialog to populate the backend dropdown.
	 *
	 * Always includes X264 (software x264 is guaranteed available in OBS).
	 * Includes NVENC/QSV/AMF only if obs_enum_encoder_types reports them.
	 *
	 * AVANATRO-VERIFY: obs_enum_encoder_types — confirm callback signature.
	 * In OBS 30 it is: void obs_enum_encoder_types(
	 *   bool (*enum_proc)(void*, const char*), void* param).
	 * Confirm it's still valid in OBS 31.x.
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
