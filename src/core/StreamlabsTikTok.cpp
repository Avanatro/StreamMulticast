/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "StreamlabsTikTok.hpp"

#include <obs-data.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <cmath>
#include <cstdlib>
#include <cstdio>

namespace smulti {

const char *const kStreamlabsUserAgent =
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
	"StreamlabsDesktop/1.17.0 Chrome/122.0.6261.156 Electron/29.3.1 Safari/537.36";

namespace {

constexpr const char *kApiBase = "https://streamlabs.com/api/v5/slobs/tiktok";
constexpr size_t kMaxConfigBytes = 64 * 1024;
constexpr int kMaxJsonDepth = 64;

/* ---- strict JSON syntax check (no values extracted) ---- */

class JsonSyntax {
public:
	explicit JsonSyntax(const std::string &t) : m_t(t) {}

	bool object_document()
	{
		skip_ws();
		if (!peek('{') || !value(0))
			return false;
		skip_ws();
		return m_i == m_t.size();
	}

private:
	bool peek(char c) const { return m_i < m_t.size() && m_t[m_i] == c; }
	void skip_ws()
	{
		while (m_i < m_t.size() &&
		       (m_t[m_i] == ' ' || m_t[m_i] == '\t' || m_t[m_i] == '\n' || m_t[m_i] == '\r'))
			++m_i;
	}
	bool literal(const char *word)
	{
		for (const char *p = word; *p; ++p, ++m_i)
			if (m_i >= m_t.size() || m_t[m_i] != *p)
				return false;
		return true;
	}
	static bool is_hex(char c)
	{
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
	}
	static int hex_value(char c)
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		return c - 'A' + 10;
	}
	/* Four hex digits after "\u"; -1 when malformed. */
	int unicode_escape()
	{
		int v = 0;
		for (int k = 0; k < 4; ++k) {
			if (m_i >= m_t.size() || !is_hex(m_t[m_i]))
				return -1;
			v = v * 16 + hex_value(m_t[m_i++]);
		}
		return v;
	}
	/* One UTF-8 sequence whose lead byte `c` was already consumed (c >= 0x80).
	 * Rejects stray continuation bytes, overlong forms, surrogates and > U+10FFFF,
	 * all of which make jansson fail and log the text near the error. */
	bool utf8_tail(unsigned char c)
	{
		int extra;
		unsigned int cp;
		if (c >= 0xC2 && c <= 0xDF) {
			extra = 1;
			cp = c & 0x1F;
		} else if (c >= 0xE0 && c <= 0xEF) {
			extra = 2;
			cp = c & 0x0F;
		} else if (c >= 0xF0 && c <= 0xF4) {
			extra = 3;
			cp = c & 0x07;
		} else {
			return false; // continuation byte, overlong 2-byte lead (C0/C1) or F5..FF
		}
		for (int k = 0; k < extra; ++k) {
			if (m_i >= m_t.size())
				return false;
			const unsigned char cc = static_cast<unsigned char>(m_t[m_i++]);
			if ((cc & 0xC0) != 0x80)
				return false;
			cp = (cp << 6) | (cc & 0x3F);
		}
		if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000))
			return false; // overlong
		if (cp >= 0xD800 && cp <= 0xDFFF)
			return false; // surrogate encoded in UTF-8
		return cp <= 0x10FFFF;
	}
	bool string()
	{
		if (!peek('"'))
			return false;
		++m_i;
		while (m_i < m_t.size()) {
			const unsigned char c = static_cast<unsigned char>(m_t[m_i++]);
			if (c == '"')
				return true;
			if (c < 0x20)
				return false;
			if (c >= 0x80) {
				if (!utf8_tail(c))
					return false;
				continue;
			}
			if (c == '\\') {
				if (m_i >= m_t.size())
					return false;
				const char e = m_t[m_i++];
				if (e == 'u') {
					const int v = unicode_escape();
					if (v <= 0) // malformed, or \u0000 (jansson refuses NUL in strings)
						return false;
					if (v >= 0xDC00 && v <= 0xDFFF)
						return false; // lone low surrogate
					if (v >= 0xD800 && v <= 0xDBFF) {
						/* a high surrogate must be followed by \u<low surrogate> */
						if (m_i + 1 >= m_t.size() || m_t[m_i] != '\\' || m_t[m_i + 1] != 'u')
							return false;
						m_i += 2;
						const int low = unicode_escape();
						if (low < 0xDC00 || low > 0xDFFF)
							return false;
					}
				} else if (e != '"' && e != '\\' && e != '/' && e != 'b' && e != 'f' && e != 'n' &&
					   e != 'r' && e != 't') {
					return false;
				}
			}
		}
		return false; // unterminated
	}
	bool digits()
	{
		const size_t start = m_i;
		while (m_i < m_t.size() && m_t[m_i] >= '0' && m_t[m_i] <= '9')
			++m_i;
		return m_i > start;
	}
	/* Integers must fit int64 and reals must be finite: jansson logs the number otherwise. */
	bool number()
	{
		const size_t start = m_i;
		const bool negative = peek('-');
		if (negative)
			++m_i;
		const size_t int_start = m_i;
		if (peek('0'))
			++m_i;
		else if (!digits())
			return false;
		const size_t int_len = m_i - int_start;
		bool real = false;
		if (peek('.')) {
			real = true;
			++m_i;
			if (!digits())
				return false;
		}
		if (peek('e') || peek('E')) {
			real = true;
			++m_i;
			if (peek('+') || peek('-'))
				++m_i;
			if (!digits())
				return false;
		}
		if (!real) {
			if (int_len > 19)
				return false;
			if (int_len == 19) {
				const char *limit = negative ? "9223372036854775808" : "9223372036854775807";
				if (m_t.compare(int_start, 19, limit) > 0)
					return false;
			}
			return true;
		}
		const std::string text = m_t.substr(start, m_i - start);
		const double d = std::strtod(text.c_str(), nullptr);
		return std::isfinite(d);
	}
	bool value(int depth)
	{
		if (depth > kMaxJsonDepth)
			return false;
		skip_ws();
		if (m_i >= m_t.size())
			return false;
		const char c = m_t[m_i];
		if (c == '{') {
			++m_i;
			skip_ws();
			if (peek('}')) {
				++m_i;
				return true;
			}
			std::vector<std::string> keys;
			for (;;) {
				skip_ws();
				const size_t key_start = m_i;
				if (!string())
					return false;
				std::string key = m_t.substr(key_start, m_i - key_start);
				for (const auto &k : keys)
					if (k == key)
						return false; // duplicate key: jansson (REJECT_DUPLICATES) would log it
				keys.push_back(std::move(key));
				skip_ws();
				if (!peek(':'))
					return false;
				++m_i;
				if (!value(depth + 1))
					return false;
				skip_ws();
				if (peek(',')) {
					++m_i;
					continue;
				}
				if (peek('}')) {
					++m_i;
					return true;
				}
				return false;
			}
		}
		if (c == '[') {
			++m_i;
			skip_ws();
			if (peek(']')) {
				++m_i;
				return true;
			}
			for (;;) {
				if (!value(depth + 1))
					return false;
				skip_ws();
				if (peek(',')) {
					++m_i;
					continue;
				}
				if (peek(']')) {
					++m_i;
					return true;
				}
				return false;
			}
		}
		if (c == '"')
			return string();
		if (c == 't')
			return literal("true");
		if (c == 'f')
			return literal("false");
		if (c == 'n')
			return literal("null");
		return number();
	}

	const std::string &m_t;
	size_t m_i = 0;
};

/* ---- obs_data helpers ---- */

/* Parse a JSON object; nullptr when the text is not exactly one well-formed object.
 * The syntax check runs first so libobs never logs a fragment of the text. */
obs_data_t *parse_object(const std::string &body)
{
	if (!streamlabs_json_is_object(body))
		return nullptr;
	return obs_data_create_from_json(body.c_str());
}

/* String or number field as text; "" when absent or of another type. */
std::string scalar_text(obs_data_t *data, const char *name)
{
	obs_data_item_t *item = obs_data_item_byname(data, name);
	if (!item)
		return {};
	std::string out;
	const enum obs_data_type type = obs_data_item_gettype(item);
	if (type == OBS_DATA_STRING) {
		const char *s = obs_data_item_get_string(item);
		out = s ? s : "";
	} else if (type == OBS_DATA_NUMBER) {
		if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT) {
			out = std::to_string(obs_data_item_get_int(item));
		} else {
			const double d = obs_data_item_get_double(item);
			if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 9.0e15)
				out = std::to_string(static_cast<long long>(d));
		}
	}
	obs_data_item_release(&item);
	return out;
}

/* Exactly a JSON true; anything else (absent, string, number) is false. */
bool bool_true(obs_data_t *data, const char *name)
{
	obs_data_item_t *item = obs_data_item_byname(data, name);
	if (!item)
		return false;
	const bool v = obs_data_item_gettype(item) == OBS_DATA_BOOLEAN && obs_data_item_get_bool(item);
	obs_data_item_release(&item);
	return v;
}

std::string nested_text(obs_data_t *data, const char *object, const char *name)
{
	obs_data_t *obj = obs_data_get_obj(data, object);
	if (!obj)
		return {};
	std::string out = scalar_text(obj, name);
	obs_data_release(obj);
	return out;
}

StreamlabsResult fail(StreamlabsError code, const std::string &message)
{
	StreamlabsResult r;
	r.ok = false;
	r.error_code = code;
	r.error_message = message;
	return r;
}

/* Status check shared by every call. Never quotes the body. */
bool status_ok(int http_status, const char *call, StreamlabsResult &out)
{
	if (http_status >= 200 && http_status < 300)
		return true;
	char buf[160];
	if (http_status == 401 || http_status == 403) {
		std::snprintf(buf, sizeof(buf),
			      "Streamlabs rejected the token (HTTP %d) during %s. Import a fresh token.", http_status,
			      call);
		out = fail(StreamlabsError::Unauthorized, buf);
	} else {
		std::snprintf(buf, sizeof(buf), "Streamlabs %s failed (HTTP %d).", call, http_status);
		out = fail(StreamlabsError::Http, buf);
	}
	return false;
}

StreamlabsResult bad_response(const char *call)
{
	return fail(StreamlabsError::BadResponse, std::string("Streamlabs ") + call + " returned an unreadable answer.");
}

std::string trim(const std::string &s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
		++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
		--e;
	return s.substr(b, e - b);
}

StreamlabsTokenImport token_error(const char *message)
{
	StreamlabsTokenImport t;
	t.error_message = message;
	return t;
}

} // namespace

/* -----------------------------------------------------------------------
 * Requests
 * ----------------------------------------------------------------------- */
StreamlabsRequest streamlabs_info_request()
{
	return {"GET", std::string(kApiBase) + "/info", {}};
}

StreamlabsRequest streamlabs_search_request(const std::string &category_name)
{
	return {"GET",
		std::string(kApiBase) + "/info?category=" + streamlabs_url_encode(streamlabs_truncate_query(category_name)),
		{}};
}

StreamlabsRequest streamlabs_start_request(const std::string &title, const std::string &category_id)
{
	return {"POST",
		std::string(kApiBase) + "/stream/start",
		{{"title", title}, {"device_platform", "win32"}, {"category", category_id}, {"audience_type", "0"}}};
}

StreamlabsRequest streamlabs_end_request(const std::string &session_id)
{
	return {"POST", std::string(kApiBase) + "/stream/" + streamlabs_url_encode(session_id) + "/end", {}};
}

std::string streamlabs_url_encode(const std::string &text)
{
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	out.reserve(text.size() * 3);
	for (unsigned char c : text) {
		const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
					c == '-' || c == '_' || c == '.' || c == '~';
		if (unreserved) {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 0x0F];
		}
	}
	return out;
}

std::string streamlabs_truncate_query(const std::string &text)
{
	size_t points = 0;
	size_t i = 0;
	while (i < text.size()) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		size_t len = 1;
		if (c >= 0xF0)
			len = 4;
		else if (c >= 0xE0)
			len = 3;
		else if (c >= 0xC0)
			len = 2;
		if (points == kStreamlabsCategoryQueryMax)
			break;
		if (i + len > text.size())
			break; // truncated multi-byte sequence: drop it
		i += len;
		++points;
	}
	return text.substr(0, i);
}

std::string streamlabs_multipart_body(const std::vector<StreamlabsFormField> &fields, const std::string &boundary)
{
	std::string body;
	for (const auto &f : fields) {
		body += "--" + boundary + "\r\n";
		body += "Content-Disposition: form-data; name=\"" + f.name + "\"\r\n\r\n";
		body += f.value + "\r\n";
	}
	body += "--" + boundary + "--\r\n";
	return body;
}

/* -----------------------------------------------------------------------
 * Responses
 * ----------------------------------------------------------------------- */
StreamlabsResult streamlabs_parse_start(int http_status, const std::string &body)
{
	StreamlabsResult r;
	if (!status_ok(http_status, "stream start", r))
		return r;
	obs_data_t *data = parse_object(body);
	if (!data)
		return bad_response("stream start");

	std::string id = scalar_text(data, "id");
	std::string rtmp = scalar_text(data, "rtmp");
	std::string key = scalar_text(data, "key");
	obs_data_release(data);

	if (id.empty() || rtmp.empty() || key.empty()) {
		const char *missing = id.empty() ? "id" : (rtmp.empty() ? "rtmp" : "key");
		return fail(StreamlabsError::MissingField,
			    std::string("Streamlabs stream start answer has no '") + missing +
				    "'. TikTok may not allow this account to go live.");
	}

	r.ok = true;
	r.id = std::move(id);
	r.rtmp = std::move(rtmp);
	r.key = std::move(key);
	return r;
}

StreamlabsResult streamlabs_parse_info(int http_status, const std::string &body)
{
	StreamlabsResult r;
	if (!status_ok(http_status, "account info", r))
		return r;
	obs_data_t *data = parse_object(body);
	if (!data)
		return bad_response("account info");
	r.ok = true;
	r.username = nested_text(data, "user", "username");
	r.application_status = nested_text(data, "application_status", "status");
	r.can_be_live = bool_true(data, "can_be_live");
	obs_data_release(data);
	return r;
}

StreamlabsResult streamlabs_parse_search(int http_status, const std::string &body)
{
	StreamlabsResult r;
	if (!status_ok(http_status, "category search", r))
		return r;
	obs_data_t *data = parse_object(body);
	if (!data)
		return bad_response("category search");
	obs_data_array_t *arr = obs_data_get_array(data, "categories");
	if (arr) {
		const size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; ++i) {
			obs_data_t *item = obs_data_array_item(arr, i);
			if (!item)
				continue;
			StreamlabsCategory c;
			c.full_name = scalar_text(item, "full_name");
			c.game_mask_id = scalar_text(item, "game_mask_id");
			obs_data_release(item);
			if (!c.full_name.empty())
				r.categories.push_back(std::move(c));
		}
		obs_data_array_release(arr);
	}
	obs_data_release(data);
	r.categories.push_back({"Other", ""});
	r.ok = true;
	return r;
}

StreamlabsResult streamlabs_parse_end(int http_status, const std::string &body)
{
	StreamlabsResult r;
	if (!status_ok(http_status, "stream end", r))
		return r;
	obs_data_t *data = parse_object(body);
	if (!data)
		return bad_response("stream end");
	const bool success = bool_true(data, "success");
	obs_data_release(data);
	if (!success)
		return fail(StreamlabsError::Rejected, "Streamlabs did not confirm the end of the TikTok session.");
	r.ok = true;
	r.success = true;
	return r;
}

StreamlabsResult streamlabs_transport_error(const std::string &what)
{
	return fail(StreamlabsError::Transport, "Could not reach Streamlabs: " + what);
}

/* -----------------------------------------------------------------------
 * Token import
 * ----------------------------------------------------------------------- */
StreamlabsTokenImport streamlabs_import_generator_config(const std::string &json_text)
{
	obs_data_t *data = parse_object(json_text);
	if (!data)
		return token_error("The file is not a valid generator config.json.");

	/* The plugin's own config (top-level "endpoints" array) is a common wrong pick in the file dialog. */
	obs_data_item_t *endpoints = obs_data_item_byname(data, "endpoints");
	const bool is_plugin_config = endpoints && obs_data_item_gettype(endpoints) == OBS_DATA_ARRAY;
	if (endpoints)
		obs_data_item_release(&endpoints);
	if (is_plugin_config) {
		obs_data_release(data);
		return token_error("This is the StreamMulticast config, not the generator's config.json. Pick the "
				   "config.json next to StreamLabsTikTokStreamKeyGenerator.exe.");
	}

	obs_data_item_t *item = obs_data_item_byname(data, "token");
	const bool is_string = item && obs_data_item_gettype(item) == OBS_DATA_STRING;
	std::string token;
	if (is_string) {
		const char *s = obs_data_item_get_string(item);
		token = trim(s ? s : "");
	}
	if (item)
		obs_data_item_release(&item);
	obs_data_release(data);

	if (!is_string)
		return token_error("The config.json has no 'token' text field. Log in once with the generator first.");
	if (token.empty())
		return token_error("The config.json 'token' is empty. Log in once with the generator first.");
	if (token.size() > kStreamlabsTokenMax)
		return token_error("The config.json 'token' is too long.");
	for (unsigned char c : token) {
		if (c < 0x21 || c > 0x7E)
			return token_error("The config.json 'token' contains unsupported characters.");
	}

	StreamlabsTokenImport ok;
	ok.ok = true;
	ok.token = std::move(token);
	return ok;
}

StreamlabsTokenImport streamlabs_import_generator_config_file(const std::string &path)
{
	if (path.empty())
		return token_error("No config.json selected.");
	/* size first, so a large file is never read into memory */
	const int64_t size = os_get_file_size(path.c_str());
	if (size > static_cast<int64_t>(kMaxConfigBytes))
		return token_error("The selected file is too large for a generator config.json.");
	char *raw = os_quick_read_utf8_file(path.c_str());
	if (!raw)
		return token_error("Could not read the selected config.json.");
	std::string text(raw);
	for (char *p = raw; *p; ++p)
		*p = 0; // wipe the copy that holds the token before freeing it
	bfree(raw);
	if (text.size() > kMaxConfigBytes)
		return token_error("The selected file is too large for a generator config.json.");
	StreamlabsTokenImport result = streamlabs_import_generator_config(text);
	/* best-effort wipe of the local copy */
	for (auto &c : text)
		c = '\0';
	return result;
}

std::string streamlabs_redact(const std::string &text, const std::vector<std::string> &secrets)
{
	std::string out = text;
	for (const auto &secret : secrets) {
		if (secret.empty())
			continue;
		size_t pos = 0;
		while ((pos = out.find(secret, pos)) != std::string::npos) {
			out.replace(pos, secret.size(), "***");
			pos += 3;
		}
	}
	return out;
}

bool streamlabs_json_is_object(const std::string &text)
{
	return JsonSyntax(text).object_document();
}

/* -----------------------------------------------------------------------
 * StreamlabsSessionBook
 * ----------------------------------------------------------------------- */
bool StreamlabsSessionBook::contains(const std::vector<std::string> &v, const std::string &id)
{
	for (const auto &x : v)
		if (x == id)
			return true;
	return false;
}

void StreamlabsSessionBook::erase(std::vector<std::string> &v, const std::string &id)
{
	for (auto it = v.begin(); it != v.end();) {
		if (*it == id)
			it = v.erase(it);
		else
			++it;
	}
}

std::string StreamlabsSessionBook::busy_with(const std::string &endpoint_id) const
{
	if (m_open_id.empty() || m_open_owner == endpoint_id)
		return {};
	return m_open_name.empty() ? std::string("another endpoint") : m_open_name;
}

StreamlabsSessionBook::StartClaim StreamlabsSessionBook::claim_for_start(const std::string &endpoint_id)
{
	StartClaim claim;
	claim.busy_with = busy_with(endpoint_id);
	if (!claim.busy_with.empty() || m_open_id.empty())
		return claim;
	claim.stale.push_back(m_open_id);
	m_open_id.clear();
	m_open_owner.clear();
	m_open_name.clear();
	return claim;
}

void StreamlabsSessionBook::on_started(const std::string &id, const std::string &owner, const std::string &owner_name)
{
	m_open_id = id;
	m_open_owner = owner;
	m_open_name = owner_name;
}

std::string StreamlabsSessionBook::release_owner(const std::string &endpoint_id)
{
	if (m_open_id.empty() || m_open_owner != endpoint_id)
		return {};
	std::string id = m_open_id;
	m_open_id.clear();
	m_open_owner.clear();
	m_open_name.clear();
	if (!contains(m_ending, id))
		m_ending.push_back(id);
	return id;
}

bool StreamlabsSessionBook::release_unadopted(const std::string &id)
{
	if (id.empty() || m_open_id != id)
		return false;
	m_open_id.clear();
	m_open_owner.clear();
	m_open_name.clear();
	return begin_end(id);
}

bool StreamlabsSessionBook::begin_end(const std::string &id)
{
	if (id.empty() || contains(m_ending, id) || contains(m_ended, id))
		return false;
	erase(m_retry, id);
	m_ending.push_back(id);
	return true;
}

void StreamlabsSessionBook::finish_end(const std::string &id, EndOutcome outcome)
{
	erase(m_ending, id);
	if (outcome == EndOutcome::Ended) {
		erase(m_retry, id);
		if (!contains(m_ended, id))
			m_ended.push_back(id);
	} else if (outcome == EndOutcome::Cancelled) {
		if (!contains(m_retry, id))
			m_retry.push_back(id);
	}
	/* Failed: neither ended nor retried here; a later start may try it again (session file). */
}

std::vector<std::string> StreamlabsSessionBook::take_for_shutdown()
{
	std::vector<std::string> ids;
	auto add = [&](const std::string &id) {
		if (!id.empty() && !contains(ids, id) && !contains(m_ended, id) && !contains(m_handed_out, id))
			ids.push_back(id);
	};
	for (const auto &id : m_retry)
		add(id);
	for (const auto &id : m_ending)
		add(id);
	add(m_open_id);
	m_open_id.clear();
	m_open_owner.clear();
	m_open_name.clear();
	m_retry.clear();
	/* The synchronous ends own them now: begin_end() refuses them, a second call skips them. */
	for (const auto &id : ids) {
		if (!contains(m_ending, id))
			m_ending.push_back(id);
		m_handed_out.push_back(id);
	}
	return ids;
}

} // namespace smulti
