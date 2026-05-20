/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "Endpoint.hpp"

#include <obs-data.h>
#include <util/base.h>

#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <sstream>
#include <iomanip>

namespace smulti {

/* -----------------------------------------------------------------------
 * UUID generation (v4-style, not cryptographically secure)
 * ----------------------------------------------------------------------- */
std::string Endpoint::generate_uuid()
{
	/* Seed once per process */
	static bool seeded = false;
	if (!seeded) {
		srand(static_cast<unsigned>(time(nullptr)));
		seeded = true;
	}

	/* Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx */
	auto rand_hex = [](int bits) -> unsigned int {
		return static_cast<unsigned int>(rand()) & ((1u << bits) - 1u);
	};

	std::ostringstream ss;
	ss << std::hex << std::setfill('0');

	ss << std::setw(8) << rand_hex(32) << '-';
	ss << std::setw(4) << rand_hex(16) << '-';
	ss << std::setw(4) << (0x4000u | rand_hex(12)) << '-'; // version 4
	ss << std::setw(4) << (0x8000u | rand_hex(14)) << '-'; // variant bits
	ss << std::setw(12) << ((static_cast<uint64_t>(rand_hex(32)) << 16) | rand_hex(16));

	return ss.str();
}

/* -----------------------------------------------------------------------
 * Factory
 * ----------------------------------------------------------------------- */
Endpoint Endpoint::make_default(const std::string &name)
{
	Endpoint ep;
	ep.id                   = generate_uuid();
	ep.name                 = name;
	ep.server_url           = "";
	ep.stream_key           = "";
	ep.encoder_backend      = EncoderBackend::X264;
	ep.video_bitrate_kbps   = 6000;
	ep.keyframe_interval_sec = 2;
	ep.audio_bitrate_kbps   = 160;
	ep.orientation          = OutputOrientation::SourceMatch;
	ep.enabled              = true;
	ep.linked_to_main       = true;
	ep.sort_order           = 0;
	return ep;
}

/* -----------------------------------------------------------------------
 * Serialization — obs_data_t round-trip
 *
 * AVANATRO-VERIFY: obs_data_create / obs_data_release ownership model.
 * obs_data_t uses reference counting. obs_data_create returns refcount=1.
 * Caller of serialize() must call obs_data_release() after use.
 * When stored in obs_data_array_t via obs_data_array_push_back, the array
 * addref's the item, so caller's release is still correct.
 * ----------------------------------------------------------------------- */
obs_data_t *Endpoint::serialize() const
{
	obs_data_t *data = obs_data_create();

	obs_data_set_string(data, "id",               id.c_str());
	obs_data_set_string(data, "name",             name.c_str());
	obs_data_set_string(data, "server_url",       server_url.c_str());
	obs_data_set_string(data, "stream_key",       stream_key.c_str());
	obs_data_set_int   (data, "encoder_backend",  static_cast<long long>(encoder_backend));
	obs_data_set_int   (data, "video_bitrate",    video_bitrate_kbps);
	obs_data_set_int   (data, "keyframe_interval",keyframe_interval_sec);
	obs_data_set_int   (data, "audio_bitrate",    audio_bitrate_kbps);
	obs_data_set_int   (data, "orientation",      static_cast<long long>(orientation));
	obs_data_set_bool  (data, "enabled",          enabled);
	obs_data_set_bool  (data, "linked_to_main",   linked_to_main);
	obs_data_set_int   (data, "sort_order",       sort_order);

	return data;
}

Endpoint Endpoint::deserialize(obs_data_t *data)
{
	if (!data)
		return Endpoint{};

	Endpoint ep;
	ep.id                   = obs_data_get_string(data, "id");
	ep.name                 = obs_data_get_string(data, "name");
	ep.server_url           = obs_data_get_string(data, "server_url");
	ep.stream_key           = obs_data_get_string(data, "stream_key");
	ep.encoder_backend      = static_cast<EncoderBackend>(
	                              static_cast<int>(obs_data_get_int(data, "encoder_backend")));
	ep.video_bitrate_kbps   = static_cast<int>(obs_data_get_int(data, "video_bitrate"));
	ep.keyframe_interval_sec = static_cast<int>(obs_data_get_int(data, "keyframe_interval"));
	ep.audio_bitrate_kbps   = static_cast<int>(obs_data_get_int(data, "audio_bitrate"));
	/* Default orientation=0=SourceMatch is also obs_data's int default for missing keys */
	ep.orientation          = static_cast<OutputOrientation>(
	                              static_cast<int>(obs_data_get_int(data, "orientation")));
	ep.enabled              = obs_data_get_bool(data, "enabled");
	ep.linked_to_main       = obs_data_get_bool(data, "linked_to_main");
	ep.sort_order           = static_cast<int>(obs_data_get_int(data, "sort_order"));

	/* Clamp bitrate to valid range */
	if (ep.video_bitrate_kbps < 500)   ep.video_bitrate_kbps = 500;
	if (ep.video_bitrate_kbps > 25000) ep.video_bitrate_kbps = 25000;
	if (ep.audio_bitrate_kbps <= 0)    ep.audio_bitrate_kbps = 160;

	return ep;
}

} // namespace smulti
