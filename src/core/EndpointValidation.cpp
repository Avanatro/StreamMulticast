/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "EndpointValidation.hpp"

namespace smulti {

namespace {

std::string trim(const std::string &in)
{
	const char *ws = " \t\r\n";
	size_t b = in.find_first_not_of(ws);
	if (b == std::string::npos)
		return "";
	size_t e = in.find_last_not_of(ws);
	return in.substr(b, e - b + 1);
}

std::string lower_ascii(std::string s)
{
	for (auto &c : s) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return s;
}

} // namespace

std::string normalize_server_url(const std::string &url)
{
	std::string s = trim(url);
	if (s.empty())
		return "";

	/* Split into "scheme://", authority (host[:port]) and the rest (path, case-sensitive). */
	std::string scheme;
	size_t pos = s.find("://");
	size_t authority_start = 0;
	if (pos != std::string::npos) {
		scheme = lower_ascii(s.substr(0, pos + 3));
		authority_start = pos + 3;
	}
	size_t path_start = s.find('/', authority_start);
	std::string authority = s.substr(authority_start, path_start == std::string::npos ? std::string::npos
											: path_start - authority_start);
	std::string path = path_start == std::string::npos ? "" : s.substr(path_start);

	while (!path.empty() && path.back() == '/')
		path.pop_back();

	authority = lower_ascii(authority);

	/* Drop the scheme's default port so "h:1935" and "h" compare equal. */
	const char *default_port = scheme == "rtmp://" ? ":1935" : scheme == "rtmps://" ? ":443" : nullptr;
	if (default_port) {
		const std::string suffix = default_port;
		if (authority.size() > suffix.size() &&
		    authority.compare(authority.size() - suffix.size(), suffix.size(), suffix) == 0)
			authority.erase(authority.size() - suffix.size());
	}

	return scheme + authority + path;
}

bool is_duplicate_target(const StreamTarget &a, const StreamTarget &b)
{
	const std::string key_a = trim(a.stream_key);
	if (key_a.empty() || key_a != trim(b.stream_key))
		return false;
	return normalize_server_url(a.server_url) == normalize_server_url(b.server_url);
}

DuplicateResult find_duplicate(const Endpoint &candidate, const std::vector<Endpoint> &all,
			       const std::optional<StreamTarget> &main_target)
{
	const StreamTarget cand{candidate.server_url, candidate.stream_key};

	for (const auto &other : all) {
		if (other.id == candidate.id || !other.enabled)
			continue;
		if (is_duplicate_target(cand, StreamTarget{other.server_url, other.stream_key}))
			return {DuplicateKind::OtherEndpoint, other.name};
	}

	if (main_target && is_duplicate_target(cand, *main_target))
		return {DuplicateKind::MainService, {}};

	return {};
}

} // namespace smulti
