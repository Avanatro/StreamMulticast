/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace smulti {

/**
 * Pure core of the optional Streamlabs TikTok key provider (build flag
 * SMULTI_STREAMLABS_TIKTOK). No network, no OBS runtime, no Qt: it only
 * describes requests and parses responses, so it is compiled and tested in
 * every build.
 *
 * The API is unofficial (it impersonates Streamlabs Desktop). Error messages
 * produced here are sanitized: they never contain the token, the stream key,
 * the RTMP URL or any part of a response body.
 */

/** Streamlabs Desktop Electron user agent. The only place this string lives. */
extern const char *const kStreamlabsUserAgent;

constexpr size_t kStreamlabsCategoryQueryMax = 25; ///< code points sent to the category search
constexpr size_t kStreamlabsTokenMax = 512;        ///< longest accepted imported token

struct StreamlabsFormField {
	std::string name;
	std::string value;
};

/** Description of one HTTP call. The adapter adds the authorization and user-agent headers. */
struct StreamlabsRequest {
	std::string method; ///< "GET" or "POST"
	std::string url;    ///< full https URL including the query
	std::vector<StreamlabsFormField> form; ///< multipart/form-data fields (POST with a body only)
};

StreamlabsRequest streamlabs_info_request();
/** Category search; the name is cut to kStreamlabsCategoryQueryMax code points and url-encoded. */
StreamlabsRequest streamlabs_search_request(const std::string &category_name);
/** Session start. `category_id` is a game_mask_id, or "" for "Other". */
StreamlabsRequest streamlabs_start_request(const std::string &title, const std::string &category_id);
StreamlabsRequest streamlabs_end_request(const std::string &session_id);

/** Percent-encode everything except RFC 3986 unreserved characters. */
std::string streamlabs_url_encode(const std::string &text);
/** First kStreamlabsCategoryQueryMax UTF-8 code points of `text`. */
std::string streamlabs_truncate_query(const std::string &text);
/** multipart/form-data body for `fields` with the given boundary. */
std::string streamlabs_multipart_body(const std::vector<StreamlabsFormField> &fields, const std::string &boundary);

enum class StreamlabsError : int {
	None = 0,
	Transport,    ///< no HTTP response (DNS, TLS, timeout, cancelled)
	Http,         ///< non-2xx status
	Unauthorized, ///< 401 / 403: the token is invalid or revoked
	BadResponse,  ///< body is not a well-formed JSON object
	MissingField, ///< a required field is absent or empty
	Rejected,     ///< well-formed answer that says no (e.g. end success=false)
	NoToken,      ///< no token imported yet
};

struct StreamlabsCategory {
	std::string full_name;
	std::string game_mask_id; ///< "" = Other
};

struct StreamlabsResult {
	bool ok = false;
	StreamlabsError error_code = StreamlabsError::None;
	std::string error_message; ///< sanitized, safe for UI and logs

	/* start */
	std::string id;
	std::string rtmp; ///< secret-bearing: never log
	std::string key;  ///< secret: never log

	/* info */
	std::string username;
	std::string application_status;
	bool can_be_live = false;

	/* end */
	bool success = false;

	/* search (always ends with {"Other", ""} when ok) */
	std::vector<StreamlabsCategory> categories;
};

StreamlabsResult streamlabs_parse_start(int http_status, const std::string &body);
StreamlabsResult streamlabs_parse_info(int http_status, const std::string &body);
StreamlabsResult streamlabs_parse_search(int http_status, const std::string &body);
StreamlabsResult streamlabs_parse_end(int http_status, const std::string &body);
/** Result for a call that produced no HTTP response. `what` must not contain secrets. */
StreamlabsResult streamlabs_transport_error(const std::string &what);

struct StreamlabsTokenImport {
	bool ok = false;
	std::string token;         ///< secret: never log or show
	std::string error_message; ///< sanitized
};

/** Extract the string field `token` from the generator's config.json text. */
StreamlabsTokenImport streamlabs_import_generator_config(const std::string &json_text);
/** Same, reading the file at `path` (UTF-8). */
StreamlabsTokenImport streamlabs_import_generator_config_file(const std::string &path);

/** Replace every non-empty secret in `text` with "***". */
std::string streamlabs_redact(const std::string &text, const std::vector<std::string> &secrets);

/**
 * Strict syntax check: `text` is exactly one JSON object (surrounding
 * whitespace allowed). Run before handing text to the libobs JSON parser,
 * whose error log quotes the text near the error and could leak a fragment
 * of a truncated key.
 */
bool streamlabs_json_is_object(const std::string &text);

} // namespace smulti
