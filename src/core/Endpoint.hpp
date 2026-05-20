/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <string>
#include <cstdint>

// Forward-declare obs_data_t to avoid pulling in all of libobs here
struct obs_data;
typedef struct obs_data obs_data_t;

namespace smulti {

/**
 * EncoderBackend — video encoder selection per endpoint.
 * Only backends that obs_enum_encoder_types reports as available
 * are offered to the user (see EncoderFactory::available_backends()).
 */
enum class EncoderBackend : int {
	X264   = 0, ///< obs_x264 — always available (software)
	NVENC  = 1, ///< jim_nvenc / ffmpeg_nvenc — NVIDIA GPU
	QSV    = 2, ///< obs_qsv11_v2 — Intel Quick Sync
	AMF    = 3, ///< h264_texture_amf — AMD AMF
};

/**
 * OutputOrientation — per-endpoint target frame shape.
 *
 * v1.0.5 adds Vertical1080x1920Letterbox to enable parallel horizontal+vertical
 * streaming (Twitch primary + TikTok/Shorts/Reels secondary).
 *
 * SourceMatch: encoder receives frames at the OBS main canvas resolution (default).
 * Vertical1080x1920Letterbox: obs_output_set_video_conversion targets 1080×1920;
 *   the 16:9 main canvas is letterboxed inside the 9:16 frame with black bars
 *   top+bottom. Mechanically reliable.
 *
 * Vertical1080x1920CenterCrop is reserved for v1.1 — requires a per-output
 * obs_view_t with a custom render path that crops the centre 9:16 slice
 * from the horizontal canvas before encoding.
 */
enum class OutputOrientation : int {
	SourceMatch                     = 0, ///< Use OBS main canvas as-is
	Vertical1080x1920Letterbox      = 1, ///< 1080×1920 with letterbox bars
	Vertical1080x1920CenterCrop     = 2, ///< v1.1 — custom view rendering, NOT YET IMPLEMENTED
};

/**
 * Endpoint — per-output configuration POD.
 *
 * UUIDs are generated on construction (new endpoint) or loaded from
 * the persisted config JSON. The UUID is the stable identity key —
 * the user-visible name is mutable.
 *
 * All fields are plain types so Endpoint remains a trivially-copyable
 * value type safe to pass between threads by copy.
 */
struct Endpoint {
	/* Identity */
	std::string id;           ///< UUID v4 string, e.g. "a1b2c3d4-..."
	std::string name;         ///< User-visible label, e.g. "Twitch Main"

	/* Connection */
	std::string server_url;   ///< Full RTMP URL, e.g. "rtmp://live.twitch.tv/app"
	std::string stream_key;   ///< Stream key (stored plain, same as OBS main stream)

	/* Video encoder */
	EncoderBackend encoder_backend = EncoderBackend::X264;
	int video_bitrate_kbps = 6000;    ///< 500 – 25000
	int keyframe_interval_sec = 2;    ///< 1 – 10

	/* Audio encoder */
	int audio_bitrate_kbps = 160;     ///< 64 / 96 / 128 / 160 / 192 / 320

	/* Output framing — v1.0.5 */
	OutputOrientation orientation = OutputOrientation::SourceMatch;

	/* Behaviour */
	bool enabled       = true;        ///< Is this endpoint active at all?
	bool linked_to_main = true;       ///< Start/stop with OBS main stream?

	/* Ordering */
	int sort_order = 0;               ///< Used by ConfigTab drag-to-reorder

	/* ----------------------------------------------------------------
	 * Serialization helpers (obs_data_t round-trip)
	 * ---------------------------------------------------------------- */

	/**
	 * Serialize this Endpoint into a new obs_data_t object.
	 * Caller is responsible for calling obs_data_release() on the returned ptr.
	 *
	 * AVANATRO-VERIFY: obs_data_create lifetime — confirm caller must obs_data_release
	 * even when the returned obs_data_t is embedded into a parent obs_data_array_t
	 * (obs_data_array_push_back does addref internally, so the extra release is correct).
	 */
	obs_data_t *serialize() const;

	/**
	 * Deserialize an Endpoint from an obs_data_t previously produced by serialize().
	 * Returns a default-constructed Endpoint with the id field empty on parse error.
	 */
	static Endpoint deserialize(obs_data_t *data);

	/**
	 * Generate a new random UUID v4 string.
	 * Uses platform rand() seeded by time — sufficient for local config identity,
	 * not for cryptographic purposes.
	 */
	static std::string generate_uuid();

	/**
	 * Create a fresh Endpoint with a generated UUID and sensible defaults.
	 */
	static Endpoint make_default(const std::string &name = "New Endpoint");

	/* ----------------------------------------------------------------
	 * Comparison (needed for registry change detection)
	 * ---------------------------------------------------------------- */
	bool operator==(const Endpoint &o) const { return id == o.id; }
	bool operator!=(const Endpoint &o) const { return id != o.id; }
};

} // namespace smulti
