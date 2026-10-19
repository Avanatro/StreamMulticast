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

/**
 * StreamlabsSessionBook — pure bookkeeping of which Streamlabs TikTok session
 * id is open, owned, being ended or ended. Not thread safe: StreamlabsClient
 * calls it under its mutex. Rules:
 *  - one open session at a time, owned by one endpoint;
 *  - a session id is ended successfully at most once (begin_end refuses an id
 *    that is being ended or was ended);
 *  - an unadopted start result ends exactly that id, and only while it is still
 *    the open, not adopted one; it never touches a newer session;
 *  - an end cut off by shutdown, a queued end dropped by shutdown, and an open
 *    session nobody adopted are all handed out once by take_for_shutdown().
 */
class StreamlabsSessionBook {
public:
	enum class EndOutcome { Ended, Failed, Cancelled };

	struct StartClaim {
		std::string busy_with;          ///< name of another endpoint owning the session ("" = free)
		std::vector<std::string> stale; ///< this endpoint's still-open id, to end before the new start
	};

	/** Name of the endpoint that owns the open session when it is not `endpoint_id`, else "". */
	std::string busy_with(const std::string &endpoint_id) const;
	/** Before a start: refuse when another endpoint owns the session, else take this endpoint's open id. */
	StartClaim claim_for_start(const std::string &endpoint_id);
	/** A start reply arrived: `id` is the open session of `owner`. */
	void on_started(const std::string &id, const std::string &owner, const std::string &owner_name);
	/** Stop of `endpoint_id`: its open id ("" = none), now marked as being ended. */
	std::string release_owner(const std::string &endpoint_id);
	/** A start result nobody adopted: true (and marked as being ended) only if `id` is still open. */
	bool release_unadopted(const std::string &id);
	/** Claim an end of `id` (stale or file id): false when it is being ended or was ended. */
	bool begin_end(const std::string &id);
	/** An end of `id` finished. Cancelled ends are handed out again at shutdown. */
	void finish_end(const std::string &id, EndOutcome outcome);
	/** Every id still to end at shutdown (open, cancelled, dropped from the queue); each once. */
	std::vector<std::string> take_for_shutdown();

	const std::string &open_id() const { return m_open_id; }
	const std::string &open_owner() const { return m_open_owner; }
	const std::string &open_name() const { return m_open_name; }

private:
	static bool contains(const std::vector<std::string> &v, const std::string &id);
	static void erase(std::vector<std::string> &v, const std::string &id);

	std::string m_open_id;
	std::string m_open_owner;
	std::string m_open_name;
	std::vector<std::string> m_ending; ///< end claimed (queued or running)
	std::vector<std::string> m_retry;  ///< end cut off by shutdown
	std::vector<std::string> m_ended;  ///< ended successfully
	std::vector<std::string> m_handed_out; ///< returned by take_for_shutdown()
};

} // namespace smulti
