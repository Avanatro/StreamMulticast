/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <string>

namespace smulti {

/**
 * TikTokBridgeConfig -- locally supplied RTMP data for a TikTok endpoint.
 *
 * This importer intentionally does not generate stream keys, authenticate to
 * TikTok, or automate third-party login flows. It only reads RTMP data from a
 * local JSON file produced by a separate helper chosen by the user.
 */
struct TikTokBridgeConfig {
	bool        ok = false;
	std::string name;
	std::string server_url;
	std::string stream_key;
	std::string expires_at;
	std::string source_path;
	std::string error_message;
};

std::string default_tiktok_bridge_path();
TikTokBridgeConfig import_tiktok_bridge_file(const std::string &path);

} // namespace smulti
