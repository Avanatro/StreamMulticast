/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "MainServiceTarget.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

namespace smulti {

std::optional<StreamTarget> read_main_service_target()
{
	/* obs_frontend_get_streaming_service() returns a borrowed pointer (no
	 * release needed, see OBSStudioAPI.cpp: `return main->GetService()`).
	 * obs_service_get_connect_info() may return NULL, and the returned
	 * strings are owned by the service: copy them immediately. */
	obs_service_t *service = obs_frontend_get_streaming_service();
	if (!service)
		return std::nullopt;

	const char *server = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	if (!server || !*server)
		return std::nullopt;

	const char *key = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);

	return StreamTarget{server, key ? key : ""};
}

} // namespace smulti
