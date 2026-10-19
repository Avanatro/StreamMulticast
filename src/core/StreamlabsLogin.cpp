/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "StreamlabsLogin.hpp"

#include <obs-data.h>

#include <cstdint>
#include <cstdio>

namespace smulti {

namespace {

constexpr const char *kLoginBase = "https://streamlabs.com/slobs/login";
constexpr const char *kExchangeBase = "https://streamlabs.com/api/v5/slobs/auth/data";
constexpr const char *kDesktopTokenPrefix = "\"apiToken\":\"";

/* ---- SHA-256 (FIPS 180-4) ---- */

constexpr uint32_t kSha256K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

void sha256_block(uint32_t h[8], const unsigned char *block)
{
	uint32_t w[64];
	for (int i = 0; i < 16; ++i)
		w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
		       (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
	for (int i = 16; i < 64; ++i) {
		const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
	for (int i = 0; i < 64; ++i) {
		const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		const uint32_t ch = (e & f) ^ (~e & g);
		const uint32_t t1 = hh + S1 + ch + kSha256K[i] + w[i];
		const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		const uint32_t t2 = S0 + maj;
		hh = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += hh;
}

/* ---- helpers ---- */

StreamlabsTokenImport token_fail(const char *message)
{
	StreamlabsTokenImport t;
	t.error_message = message;
	return t;
}

StreamlabsLoginCallback callback_kind(StreamlabsCallbackKind kind, const char *message = "")
{
	StreamlabsLoginCallback c;
	c.kind = kind;
	c.error_message = message;
	return c;
}

bool is_hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return c - 'A' + 10;
}

/* application/x-www-form-urlencoded value; false on a broken %XX escape. */
bool percent_decode(const std::string &in, std::string &out)
{
	out.clear();
	for (size_t i = 0; i < in.size(); ++i) {
		const char c = in[i];
		if (c == '+') {
			out += ' ';
		} else if (c == '%') {
			if (i + 2 >= in.size() || !is_hex(in[i + 1]) || !is_hex(in[i + 2]))
				return false;
			out += static_cast<char>(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2]));
			i += 2;
		} else {
			out += c;
		}
	}
	return true;
}

void wipe(std::string &s)
{
	for (auto &c : s)
		c = '\0';
	s.clear();
}

/* Exactly a JSON true; anything else is false. */
bool bool_true(obs_data_t *data, const char *name)
{
	obs_data_item_t *item = obs_data_item_byname(data, name);
	if (!item)
		return false;
	const bool v = obs_data_item_gettype(item) == OBS_DATA_BOOLEAN && obs_data_item_get_bool(item);
	obs_data_item_release(&item);
	return v;
}

/* A string field; false when absent or of another type. */
bool string_field(obs_data_t *data, const char *name, std::string &out)
{
	obs_data_item_t *item = obs_data_item_byname(data, name);
	if (!item)
		return false;
	const bool is_string = obs_data_item_gettype(item) == OBS_DATA_STRING;
	if (is_string) {
		const char *s = obs_data_item_get_string(item);
		out = s ? s : "";
	}
	obs_data_item_release(&item);
	return is_string;
}

} // namespace

/* -----------------------------------------------------------------------
 * PKCE
 * ----------------------------------------------------------------------- */
std::string streamlabs_sha256(const std::string &data)
{
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	const size_t n = data.size();
	const unsigned char *p = reinterpret_cast<const unsigned char *>(data.data());
	size_t full = n / 64;
	for (size_t i = 0; i < full; ++i)
		sha256_block(h, p + i * 64);

	/* padding: 0x80, zeros, 64-bit big-endian bit length */
	unsigned char tail[128] = {};
	const size_t rest = n % 64;
	for (size_t i = 0; i < rest; ++i)
		tail[i] = p[full * 64 + i];
	tail[rest] = 0x80;
	const size_t tail_len = rest < 56 ? 64 : 128;
	const uint64_t bits = static_cast<uint64_t>(n) * 8;
	for (int i = 0; i < 8; ++i)
		tail[tail_len - 1 - i] = static_cast<unsigned char>(bits >> (8 * i));
	sha256_block(h, tail);
	if (tail_len == 128)
		sha256_block(h, tail + 64);

	std::string out(32, '\0');
	for (int i = 0; i < 8; ++i) {
		out[i * 4] = static_cast<char>(h[i] >> 24);
		out[i * 4 + 1] = static_cast<char>(h[i] >> 16);
		out[i * 4 + 2] = static_cast<char>(h[i] >> 8);
		out[i * 4 + 3] = static_cast<char>(h[i]);
	}
	return out;
}

std::string streamlabs_base64url(const std::string &bytes)
{
	static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	std::string out;
	out.reserve((bytes.size() * 4 + 2) / 3);
	size_t i = 0;
	const auto at = [&](size_t k) { return static_cast<uint32_t>(static_cast<unsigned char>(bytes[k])); };
	for (; i + 3 <= bytes.size(); i += 3) {
		const uint32_t v = (at(i) << 16) | (at(i + 1) << 8) | at(i + 2);
		out += alphabet[(v >> 18) & 63];
		out += alphabet[(v >> 12) & 63];
		out += alphabet[(v >> 6) & 63];
		out += alphabet[v & 63];
	}
	const size_t rest = bytes.size() - i;
	if (rest == 1) {
		const uint32_t v = at(i) << 16;
		out += alphabet[(v >> 18) & 63];
		out += alphabet[(v >> 12) & 63];
	} else if (rest == 2) {
		const uint32_t v = (at(i) << 16) | (at(i + 1) << 8);
		out += alphabet[(v >> 18) & 63];
		out += alphabet[(v >> 12) & 63];
		out += alphabet[(v >> 6) & 63];
	}
	return out;
}

StreamlabsPkce streamlabs_login_pkce(const unsigned char *random, size_t size)
{
	StreamlabsPkce p;
	if (!random || size != kStreamlabsPkceRandomBytes)
		return p;
	static const char *hex = "0123456789abcdef";
	p.verifier.reserve(size * 2);
	for (size_t i = 0; i < size; ++i) {
		p.verifier += hex[random[i] >> 4];
		p.verifier += hex[random[i] & 0x0F];
	}
	std::string digest = streamlabs_sha256(p.verifier);
	p.challenge = streamlabs_base64url(digest);
	wipe(digest);
	return p;
}

std::string streamlabs_login_url(int port, const std::string &challenge)
{
	if (port < 1 || port > 65535 || challenge.empty())
		return {};
	return std::string(kLoginBase) +
	       "?skip_splash=true&external=electron&tiktok&force_verify&origin=slobs&port=" + std::to_string(port) +
	       "&code_challenge=" + streamlabs_url_encode(challenge) + "&code_flow=true";
}

/* -----------------------------------------------------------------------
 * Local callback
 * ----------------------------------------------------------------------- */
StreamlabsLoginCallback streamlabs_login_parse_callback(const std::string &request)
{
	const size_t line_end = request.find("\r\n");
	if (line_end == std::string::npos) {
		/* An over-long request line or binary noise is never a callback. */
		if (request.size() >= kStreamlabsCallbackMaxBytes)
			return callback_kind(StreamlabsCallbackKind::NotCallback);
		return callback_kind(StreamlabsCallbackKind::Incomplete);
	}
	if (line_end >= kStreamlabsCallbackMaxBytes)
		return callback_kind(StreamlabsCallbackKind::NotCallback);

	const std::string line = request.substr(0, line_end);
	const size_t sp1 = line.find(' ');
	const size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
	if (sp1 == std::string::npos || sp2 == std::string::npos || line.compare(0, sp1, "GET") != 0 ||
	    line.compare(sp2 + 1, 5, "HTTP/") != 0)
		return callback_kind(StreamlabsCallbackKind::NotCallback);
	const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
	if (target.empty() || target[0] != '/')
		return callback_kind(StreamlabsCallbackKind::NotCallback);

	const size_t q = target.find('?');
	if (q == std::string::npos)
		return callback_kind(StreamlabsCallbackKind::NotCallback);
	std::string query = target.substr(q + 1);
	const size_t hash = query.find('#');
	if (hash != std::string::npos)
		query.resize(hash);

	bool has_success = false, has_code = false, decode_ok = true;
	std::string success, code;
	size_t pos = 0;
	while (pos <= query.size()) {
		size_t amp = query.find('&', pos);
		if (amp == std::string::npos)
			amp = query.size();
		const std::string pair = query.substr(pos, amp - pos);
		const size_t eq = pair.find('=');
		const std::string name = pair.substr(0, eq);
		const std::string raw = eq == std::string::npos ? std::string() : pair.substr(eq + 1);
		if (name == "success") {
			has_success = true;
			decode_ok = percent_decode(raw, success) && decode_ok;
		} else if (name == "code") {
			has_code = true;
			std::string decoded;
			decode_ok = percent_decode(raw, decoded) && decode_ok;
			wipe(code);
			code = decoded;
			wipe(decoded);
		}
		pos = amp + 1;
	}
	wipe(query);

	if (!has_success && !has_code)
		return callback_kind(StreamlabsCallbackKind::NotCallback);

	const char *failed = "Streamlabs did not complete the login. Try again.";
	bool code_ok = decode_ok && !code.empty() && code.size() <= kStreamlabsLoginCodeMax;
	for (unsigned char c : code)
		if (c < 0x21 || c > 0x7E)
			code_ok = false;
	if (!has_success || success != "true" || !code_ok) {
		wipe(code);
		return callback_kind(StreamlabsCallbackKind::Failed, failed);
	}
	StreamlabsLoginCallback ok;
	ok.kind = StreamlabsCallbackKind::Code;
	ok.code = std::move(code);
	return ok;
}

/* -----------------------------------------------------------------------
 * Code exchange
 * ----------------------------------------------------------------------- */
StreamlabsRequest streamlabs_login_exchange_request(const std::string &verifier, const std::string &code)
{
	return {"GET",
		std::string(kExchangeBase) + "?code_verifier=" + streamlabs_url_encode(verifier) +
			"&code=" + streamlabs_url_encode(code),
		{}};
}

StreamlabsTokenImport streamlabs_login_parse_exchange(int http_status, const std::string &body)
{
	if (http_status != 200) {
		char buf[160];
		if (http_status == 400 || http_status == 401 || http_status == 403)
			std::snprintf(buf, sizeof(buf),
				      "Streamlabs refused the login (HTTP %d). Start the login again.", http_status);
		else
			std::snprintf(buf, sizeof(buf), "Streamlabs login failed (HTTP %d).", http_status);
		return token_fail(buf);
	}
	/* The syntax check runs first so libobs never logs a fragment of the token. */
	if (!streamlabs_json_is_object(body))
		return token_fail("Streamlabs login returned an unreadable answer.");
	obs_data_t *data = obs_data_create_from_json(body.c_str());
	if (!data)
		return token_fail("Streamlabs login returned an unreadable answer.");

	const bool success = bool_true(data, "success");
	std::string token;
	bool is_string = false;
	if (success) {
		obs_data_t *inner = obs_data_get_obj(data, "data");
		if (inner) {
			is_string = string_field(inner, "oauth_token", token);
			obs_data_release(inner);
		}
	}
	obs_data_release(data);

	if (!success)
		return token_fail("Streamlabs did not confirm the login. Start the login again.");
	if (!is_string || !streamlabs_token_text_ok(token)) {
		wipe(token);
		return token_fail("Streamlabs login answer has no usable token.");
	}
	StreamlabsTokenImport ok;
	ok.ok = true;
	ok.token = std::move(token);
	return ok;
}

/* -----------------------------------------------------------------------
 * Streamlabs Desktop local storage
 * ----------------------------------------------------------------------- */
StreamlabsTokenImport streamlabs_desktop_scan_token(const std::string &text)
{
	const std::string prefix = kDesktopTokenPrefix;
	size_t best_start = std::string::npos;
	size_t best_len = 0;
	size_t pos = 0;
	while ((pos = text.find(prefix, pos)) != std::string::npos) {
		const size_t start = pos + prefix.size();
		size_t end = start;
		while (end < text.size() && is_hex(text[end]))
			++end;
		const size_t len = end - start;
		if (end < text.size() && text[end] == '"' && len >= kStreamlabsDesktopTokenMin &&
		    len <= kStreamlabsDesktopTokenMax) {
			best_start = start;
			best_len = len;
		}
		pos = start;
	}
	if (best_start == std::string::npos)
		return token_fail("No Streamlabs Desktop login was found. Log in to Streamlabs Desktop first, or use "
				  "\"Log in with Streamlabs\".");
	StreamlabsTokenImport ok;
	ok.ok = true;
	ok.token = text.substr(best_start, best_len);
	return ok;
}

} // namespace smulti
