/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "EncoderFactory.hpp"
#include "../plugin-support.h"

#include <obs.h>
#include <obs-data.h>

namespace smulti {

/* -----------------------------------------------------------------------
 * Encoder type ID strings
 * ----------------------------------------------------------------------- */
std::string EncoderFactory::encoder_type_id(EncoderBackend backend)
{
	switch (backend) {
	case EncoderBackend::X264:  return "obs_x264";
	case EncoderBackend::NVENC: return "jim_nvenc";       // preferred; fallback: "ffmpeg_nvenc"
	case EncoderBackend::QSV:   return "obs_qsv11_v2";
	case EncoderBackend::AMF:   return "h264_texture_amf";
	default:                    return "obs_x264";
	}
}

std::string EncoderFactory::backend_label(EncoderBackend backend)
{
	switch (backend) {
	case EncoderBackend::X264:  return "x264 (Software)";
	case EncoderBackend::NVENC: return "NVENC (NVIDIA GPU)";
	case EncoderBackend::QSV:   return "QSV (Intel GPU)";
	case EncoderBackend::AMF:   return "AMF (AMD GPU)";
	default:                    return "x264 (Software)";
	}
}

/* -----------------------------------------------------------------------
 * is_encoder_available — checks the OBS encoder registry
 *
 * Verified for OBS 31.x: obs_enum_encoder_types uses INDEXED iteration:
 *   EXPORT bool obs_enum_encoder_types(size_t idx, const char **id);
 * Returns true while a valid encoder exists at that index.
 * ----------------------------------------------------------------------- */
bool EncoderFactory::is_encoder_available(const std::string &type_id)
{
	size_t      idx = 0;
	const char *id  = nullptr;
	while (obs_enum_encoder_types(idx++, &id)) {
		if (id && type_id == id)
			return true;
	}
	return false;
}

/* -----------------------------------------------------------------------
 * available_backends()
 * ----------------------------------------------------------------------- */
std::vector<EncoderBackend> EncoderFactory::available_backends()
{
	std::vector<EncoderBackend> result;

	/* x264 is always available in stock OBS builds */
	result.push_back(EncoderBackend::X264);

	/* Check GPU encoders — preferred IDs first, then fallbacks */
	if (is_encoder_available("jim_nvenc") || is_encoder_available("ffmpeg_nvenc"))
		result.push_back(EncoderBackend::NVENC);

	if (is_encoder_available("obs_qsv11_v2") || is_encoder_available("obs_qsv11"))
		result.push_back(EncoderBackend::QSV);

	if (is_encoder_available("h264_texture_amf") || is_encoder_available("ffmpeg_amf"))
		result.push_back(EncoderBackend::AMF);

	return result;
}

/* -----------------------------------------------------------------------
 * create_video_encoder()
 *
 * AVANATRO-VERIFY: obs_video_encoder_create signature.
 * OBS 30.x: obs_encoder_t* obs_video_encoder_create(
 *   const char *id, const char *name,
 *   obs_data_t *settings, obs_data_t *hotkey_data);
 * Some OBS 31.x builds add a "mixer_idx" param — check obs-encoder.h.
 * Using 4-arg form here (no mixer index) for broadest compatibility.
 * ----------------------------------------------------------------------- */
obs_encoder_t *EncoderFactory::create_video_encoder(const Endpoint &ep,
                                                     const std::string &name_hint) const
{
	/* Resolve actual type ID with fallback */
	std::string type_id = encoder_type_id(ep.encoder_backend);

	/* NVENC fallback chain: jim_nvenc → ffmpeg_nvenc */
	if (ep.encoder_backend == EncoderBackend::NVENC &&
	    !is_encoder_available(type_id)) {
		type_id = "ffmpeg_nvenc";
		if (!is_encoder_available(type_id)) {
			obs_log(LOG_WARNING,
			        "EncoderFactory: NVENC not available — falling back to x264");
			type_id = "obs_x264";
		}
	}

	/* QSV fallback */
	if (ep.encoder_backend == EncoderBackend::QSV &&
	    !is_encoder_available(type_id)) {
		type_id = "obs_qsv11";
		if (!is_encoder_available(type_id)) {
			obs_log(LOG_WARNING,
			        "EncoderFactory: QSV not available — falling back to x264");
			type_id = "obs_x264";
		}
	}

	/* AMF fallback */
	if (ep.encoder_backend == EncoderBackend::AMF &&
	    !is_encoder_available(type_id)) {
		obs_log(LOG_WARNING,
		        "EncoderFactory: AMF not available — falling back to x264");
		type_id = "obs_x264";
	}

	/* Build encoder settings */
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", ep.video_bitrate_kbps);
	obs_data_set_int(settings, "keyint_sec", ep.keyframe_interval_sec);

	/* x264-specific: prefer "veryfast" preset for low-latency live streaming */
	if (type_id == "obs_x264") {
		obs_data_set_string(settings, "preset", "veryfast");
		obs_data_set_string(settings, "profile", "high");
		obs_data_set_string(settings, "tune",    "zerolatency");
		obs_data_set_int   (settings, "buffer_size", ep.video_bitrate_kbps);
	}

	/* NVENC-specific */
	if (type_id == "jim_nvenc" || type_id == "ffmpeg_nvenc") {
		obs_data_set_string(settings, "preset",  "hq");
		obs_data_set_string(settings, "profile", "high");
		obs_data_set_int   (settings, "bf",      2); // B-frames
		obs_data_set_int   (settings, "buffer_size", ep.video_bitrate_kbps);
	}

	std::string encoder_name = name_hint + "_video_" + ep.id;
	// AVANATRO-VERIFY: obs_video_encoder_create — confirm 4-arg signature in OBS 31.
	obs_encoder_t *enc = obs_video_encoder_create(
		type_id.c_str(),
		encoder_name.c_str(),
		settings,
		nullptr /* hotkey_data */
	);

	obs_data_release(settings);

	if (!enc) {
		obs_log(LOG_ERROR, "EncoderFactory: obs_video_encoder_create failed for type '%s'",
		        type_id.c_str());
		return nullptr;
	}

	/* Attach to the main OBS video output (obs_get_video())
	 * AVANATRO-VERIFY: obs_encoder_set_video — confirm this must be called
	 * before obs_output_set_video_encoder, or whether set_video_encoder
	 * does it internally.  Pattern from OBS source: set_video before attach. */
	obs_encoder_set_video(enc, obs_get_video());

	obs_log(LOG_INFO, "EncoderFactory: created video encoder '%s' (type=%s, bitrate=%d kbps)",
	        encoder_name.c_str(), type_id.c_str(), ep.video_bitrate_kbps);
	return enc;
}

/* -----------------------------------------------------------------------
 * create_audio_encoder()
 *
 * AVANATRO-VERIFY: obs_audio_encoder_create — the mixer_idx param (5th arg).
 * mixer_idx=0 means OBS's default audio mix (same as main stream output).
 * Confirm this is the correct way to grab shared OBS audio in OBS 31.x.
 * ----------------------------------------------------------------------- */
obs_encoder_t *EncoderFactory::create_audio_encoder(const Endpoint &ep,
                                                     const std::string &name_hint) const
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", ep.audio_bitrate_kbps);

	std::string encoder_name = name_hint + "_audio_" + ep.id;

	// AVANATRO-VERIFY: obs_audio_encoder_create signature — 5 args in OBS 30:
	// (id, name, settings, mixer_idx, hotkey_data).  Confirm OBS 31 is identical.
	obs_encoder_t *enc = obs_audio_encoder_create(
		"ffmpeg_aac",
		encoder_name.c_str(),
		settings,
		0,       /* mixer_idx = 0 → main OBS audio mix */
		nullptr  /* hotkey_data */
	);

	obs_data_release(settings);

	if (!enc) {
		obs_log(LOG_ERROR, "EncoderFactory: obs_audio_encoder_create failed");
		return nullptr;
	}

	obs_encoder_set_audio(enc, obs_get_audio());

	obs_log(LOG_INFO, "EncoderFactory: created audio encoder '%s' (%d kbps AAC)",
	        encoder_name.c_str(), ep.audio_bitrate_kbps);
	return enc;
}

} // namespace smulti
