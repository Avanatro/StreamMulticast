/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "ObsServiceImport.hpp"
#include "../plugin-support.h"

#include <obs-data.h>
#include <util/platform.h>

#include <fstream>
#include <string>

namespace smulti {

/* -----------------------------------------------------------------------
 * Known service → RTMP URL mapping
 *
 * OBS's rtmp-services plugin ships a full services.json (~50 platforms),
 * but for v1.0.6 we hardcode the major ones to avoid having to parse
 * OBS's data folder.  Unknown services fall through with an error.
 *
 * Servers are taken from OBS's services.json (rtmp_common service type)
 * as of OBS 31.x.
 * ----------------------------------------------------------------------- */
static std::string resolve_service_server(const std::string &service_name)
{
	if (service_name == "Twitch")
		return "rtmp://live.twitch.tv/app";
	if (service_name == "YouTube - HLS")
		return "rtmps://a.rtmps.youtube.com/live2";
	if (service_name == "YouTube - RTMPS")
		return "rtmps://a.rtmps.youtube.com/live2";
	if (service_name == "Facebook Live")
		return "rtmps://live-api-s.facebook.com:443/rtmp";
	if (service_name == "Kick")
		return "rtmps://fa723fc1b171.global-contribute.live-video.net/app";
	if (service_name == "Trovo")
		return "rtmp://livepush.trovo.live/push";
	if (service_name == "Restream.io - RTMP")
		return "rtmp://live.restream.io/live";
	return {};
}

/* -----------------------------------------------------------------------
 * Read [Basic] Profile=... from global.ini (plain INI parsing)
 * ----------------------------------------------------------------------- */
static std::string find_active_profile(const std::string &globalIniPath)
{
	std::ifstream in(globalIniPath);
	if (!in)
		return {};

	std::string line;
	bool in_basic_section = false;
	while (std::getline(in, line)) {
		/* Trim trailing \r (Windows line endings) */
		if (!line.empty() && line.back() == '\r')
			line.pop_back();

		if (line.empty())
			continue;

		if (line.front() == '[') {
			in_basic_section = (line == "[Basic]");
			continue;
		}

		if (in_basic_section && line.rfind("Profile=", 0) == 0) {
			return line.substr(8);
		}
	}
	return {};
}

/* -----------------------------------------------------------------------
 * import_from_active_obs_profile
 * ----------------------------------------------------------------------- */
ObsServiceConfig import_from_active_obs_profile()
{
	ObsServiceConfig cfg;

	/* 1. Find OBS config base directory */
	char buf[1024] = {};
	int written = os_get_config_path(buf, sizeof(buf), "obs-studio");
	if (written <= 0) {
		cfg.error_message = "Cannot find OBS config directory";
		return cfg;
	}
	std::string base(buf);

	/* 2. Read the active profile name.
	 *
	 * OBS 30+ moved per-user preferences (including the active Profile=)
	 * from global.ini to user.ini.  We try user.ini first, fall back to
	 * global.ini for older installations.
	 *
	 * Both files use the same INI layout: [Basic] Profile=<name>. */
	std::string userIni   = base + "/user.ini";
	std::string globalIni = base + "/global.ini";

	std::string profile = find_active_profile(userIni);
	std::string source  = "user.ini";
	if (profile.empty()) {
		profile = find_active_profile(globalIni);
		source  = "global.ini";
	}
	if (profile.empty()) {
		cfg.error_message = "No active OBS profile found in user.ini or global.ini";
		return cfg;
	}
	obs_log(LOG_INFO,
	        "ObsServiceImport: active profile '%s' (from %s)",
	        profile.c_str(), source.c_str());

	/* 3. Load service.json from the active profile */
	std::string serviceJson = base + "/basic/profiles/" + profile + "/service.json";
	obs_data_t *data = obs_data_create_from_json_file(serviceJson.c_str());
	if (!data) {
		cfg.error_message = "service.json not found or invalid in profile '" + profile + "'";
		return cfg;
	}

	/* 4. Extract service settings */
	obs_data_t *settings = obs_data_get_obj(data, "settings");
	if (settings) {
		const char *svc    = obs_data_get_string(settings, "service");
		const char *server = obs_data_get_string(settings, "server");
		const char *key    = obs_data_get_string(settings, "key");

		cfg.service_name = svc    ? svc    : "";
		cfg.server_url   = server ? server : "";
		cfg.stream_key   = key    ? key    : "";

		obs_data_release(settings);
	}
	obs_data_release(data);

	/* 5. Resolve "auto" / empty server via service-name → RTMP-URL map */
	if (cfg.server_url == "auto" || cfg.server_url.empty()) {
		std::string resolved = resolve_service_server(cfg.service_name);
		if (!resolved.empty()) {
			cfg.server_url = resolved;
		}
	}

	/* 6. Validate */
	if (cfg.stream_key.empty()) {
		cfg.error_message = "No stream key in profile '" + profile +
		                    "' (have you connected an account in OBS?)";
		return cfg;
	}
	if (cfg.server_url.empty()) {
		cfg.error_message = "Unknown service '" + cfg.service_name +
		                    "' — please enter the server URL manually";
		return cfg;
	}

	cfg.ok = true;
	obs_log(LOG_INFO,
	        "ObsServiceImport: imported '%s' from profile '%s' (server=%s)",
	        cfg.service_name.c_str(), profile.c_str(), cfg.server_url.c_str());
	return cfg;
}

} // namespace smulti
