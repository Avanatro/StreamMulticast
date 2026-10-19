/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "StreamlabsTikTok.hpp"

#include <cstddef>
#include <string>

namespace smulti {

/**
 * Pure core of the Streamlabs login inside OBS (TASK-32, build flag
 * SMULTI_STREAMLABS_TIKTOK). No network, no sockets, no OBS runtime, no Qt:
 * it builds the PKCE pair, the login URL and the code exchange request, and
 * parses the local callback request, the exchange answer and Streamlabs
 * Desktop's local storage text. Compiled and tested in every build.
 *
 * Flow (same as the Streamlabs Desktop / generator login):
 *  1. verifier = 64 random bytes as lowercase hex (128 chars);
 *     challenge = base64url(sha256(verifier as ASCII)) without padding.
 *  2. The browser opens streamlabs_login_url(port, challenge); Streamlabs
 *     redirects to http://127.0.0.1:<port>/?success=true&code=<code>.
 *  3. GET streamlabs_login_exchange_request(verifier, code) answers
 *     {"success":true,"data":{"oauth_token":"<token>"}}.
 *
 * Error messages are sanitized: they never contain the code, the verifier,
 * the token or any part of a request or response body.
 */

constexpr size_t kStreamlabsPkceRandomBytes = 64;      ///< random bytes behind the verifier
constexpr size_t kStreamlabsCallbackMaxBytes = 8192;  ///< largest local callback request read
constexpr size_t kStreamlabsLoginCodeMax = 2048;      ///< longest accepted login code
constexpr size_t kStreamlabsDesktopTokenMin = 16;     ///< Streamlabs Desktop apiToken length bounds (hex)
constexpr size_t kStreamlabsDesktopTokenMax = 128;
constexpr int kStreamlabsLoginTimeoutSeconds = 300;   ///< browser login wait

struct StreamlabsPkce {
	std::string verifier;  ///< secret: never log
	std::string challenge; ///< sent in the login URL
};

/** PKCE pair from exactly kStreamlabsPkceRandomBytes random bytes; empty pair otherwise. */
StreamlabsPkce streamlabs_login_pkce(const unsigned char *random, size_t size);
/** SHA-256 digest (32 raw bytes) of `data`. */
std::string streamlabs_sha256(const std::string &data);
/** RFC 4648 base64url of `bytes`, without '=' padding. */
std::string streamlabs_base64url(const std::string &bytes);

/** Browser login URL for the local callback port (1..65535); "" for a bad port or challenge. */
std::string streamlabs_login_url(int port, const std::string &challenge);

enum class StreamlabsCallbackKind : int {
	Incomplete = 0, ///< the request line has not fully arrived; read more
	NotCallback,    ///< another request (favicon, other method, garbage): answer 404, keep waiting
	Failed,         ///< a callback that says no or carries no usable code: the login failed
	Code,           ///< success=true with a usable code
};

struct StreamlabsLoginCallback {
	StreamlabsCallbackKind kind = StreamlabsCallbackKind::Incomplete;
	std::string code;          ///< secret: only for kind == Code
	std::string error_message; ///< sanitized, for kind == Failed
};

/**
 * Parse the start of an HTTP request received on the local callback port.
 * Only `GET <any path>?<query> HTTP/x` counts. A query with `success` or
 * `code` is the callback: Code when success is exactly "true" and the
 * percent-decoded code is 1..kStreamlabsLoginCodeMax printable ASCII
 * characters, else Failed.
 */
StreamlabsLoginCallback streamlabs_login_parse_callback(const std::string &request);

/** GET https://streamlabs.com/api/v5/slobs/auth/data?code_verifier=<v>&code=<code>, url-encoded. No auth header. */
StreamlabsRequest streamlabs_login_exchange_request(const std::string &verifier, const std::string &code);
/** HTTP 200 + JSON success true + data.oauth_token (non-empty printable ASCII, at most kStreamlabsTokenMax). */
StreamlabsTokenImport streamlabs_login_parse_exchange(int http_status, const std::string &body);

/**
 * Last `"apiToken":"<hex>"` in `text` (Streamlabs Desktop leveldb .log
 * contents) whose value is kStreamlabsDesktopTokenMin..Max hex characters;
 * occurrences with other values are skipped.
 */
StreamlabsTokenImport streamlabs_desktop_scan_token(const std::string &text);

} // namespace smulti
