/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <string>

namespace smulti {

/**
 * ObsServiceConfig — result of importing the active OBS profile's
 * streaming service configuration.
 *
 * v1.0.6 — implements the user-requested "auto-login" flow without
 * the OAuth maintenance tax: we read the stream key from OBS's own
 * profile config (where OBS already obtained it via its native
 * "Connect Account" OAuth flow for Twitch / YouTube / Facebook).
 *
 * If the user has OBS-native Twitch configured, we get the Twitch
 * stream key for free.  For multistream setups, the user clicks
 * "Import from OBS" for the platform that matches OBS-native, then
 * enters keys manually for any additional platforms.
 */
struct ObsServiceConfig {
	bool        ok = false;
	std::string service_name;     ///< "Twitch" / "YouTube - HLS" / "Custom..." / etc.
	std::string server_url;       ///< Resolved RTMP URL
	std::string stream_key;       ///< Plain text (same as OBS stores it)
	std::string error_message;    ///< Set when ok=false
};

/**
 * Read the active OBS profile's service settings.
 *
 *   %APPDATA%\obs-studio\global.ini      → finds [Basic] Profile=<name>
 *   %APPDATA%\obs-studio\basic\profiles\<name>\service.json
 *       → settings.service / settings.server / settings.key
 *
 * For services with server="auto" (Twitch/YouTube), we map the service
 * name to a canonical RTMP URL.  Custom RTMP services pass through.
 */
ObsServiceConfig import_from_active_obs_profile();

} // namespace smulti
