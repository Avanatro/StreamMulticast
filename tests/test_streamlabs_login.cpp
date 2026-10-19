// Streamlabs login (TASK-32): PKCE, login URL, local callback parsing, code exchange, Streamlabs Desktop
// token scan. Pure core only: no network, no browser, no real Streamlabs Desktop files.
// Every token, code and verifier below is a fake fixture value.
#include "TestRunner.hpp"
#include "core/StreamlabsLogin.hpp"

#include <util/base.h>

#include <cstdarg>
#include <cstdio>
#include <string>

using smulti::StreamlabsCallbackKind;
using smulti::StreamlabsLoginCallback;
using smulti::StreamlabsPkce;
using smulti::StreamlabsRequest;
using smulti::StreamlabsTokenImport;

namespace {

bool contains(const std::string &haystack, const std::string &needle)
{
	return haystack.find(needle) != std::string::npos;
}

std::string g_login_log;

void capture_login_log(int, const char *format, va_list args, void *)
{
	char buf[1024];
	std::vsnprintf(buf, sizeof(buf), format, args);
	g_login_log += buf;
	g_login_log += '\n';
}

std::string hex_of(const std::string &bytes)
{
	static const char *hex = "0123456789abcdef";
	std::string out;
	for (unsigned char c : bytes) {
		out += hex[c >> 4];
		out += hex[c & 0x0F];
	}
	return out;
}

const char *kFakeCode = "FAKE-CODE-abc123";
const char *kFakeOauth = "FAKE-OAUTH-TOKEN-not-real";

} // namespace

SMULTI_TEST(streamlabs_login_pkce_challenge_known_vector)
{
	/* Expected values computed independently with Python:
	 *   v = bytes(range(64)).hex()
	 *   base64.urlsafe_b64encode(hashlib.sha256(v.encode()).digest()).decode().rstrip('=') */
	unsigned char random[64];
	for (int i = 0; i < 64; ++i)
		random[i] = static_cast<unsigned char>(i);
	const StreamlabsPkce p = smulti::streamlabs_login_pkce(random, sizeof(random));
	EXPECT_EQ(p.verifier.size(), static_cast<size_t>(128));
	EXPECT_EQ(p.verifier, std::string("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
					  "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"));
	EXPECT_EQ(p.challenge, std::string("kLgmGRlI3bYteAF4oKThCzunRnhCU7XH8bgVrwIIxUQ"));
	EXPECT_TRUE(!contains(p.challenge, "="));
	EXPECT_TRUE(!contains(p.challenge, "+"));
	EXPECT_TRUE(!contains(p.challenge, "/"));

	/* SHA-256 itself against the FIPS 180-2 vectors (also from Python hashlib) */
	EXPECT_EQ(hex_of(smulti::streamlabs_sha256("abc")),
		  std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	EXPECT_EQ(hex_of(smulti::streamlabs_sha256(std::string(1000, 'a'))),
		  std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
	EXPECT_EQ(smulti::streamlabs_base64url(smulti::streamlabs_sha256("abc")),
		  std::string("ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qctBD_YfIAFa0"));

	/* the wrong amount of randomness gives no pair */
	const StreamlabsPkce short_pair = smulti::streamlabs_login_pkce(random, 16);
	EXPECT_TRUE(short_pair.verifier.empty());
	EXPECT_TRUE(short_pair.challenge.empty());
}

SMULTI_TEST(streamlabs_login_url_has_port_and_challenge)
{
	const std::string url = smulti::streamlabs_login_url(53123, "kLgmGRlI3bYteAF4oKThCzunRnhCU7XH8bgVrwIIxUQ");
	EXPECT_EQ(url, std::string("https://streamlabs.com/slobs/login?skip_splash=true&external=electron&tiktok"
				   "&force_verify&origin=slobs&port=53123"
				   "&code_challenge=kLgmGRlI3bYteAF4oKThCzunRnhCU7XH8bgVrwIIxUQ&code_flow=true"));
	EXPECT_TRUE(contains(smulti::streamlabs_login_url(1, "x"), "&port=1&"));
	/* no port, no URL */
	EXPECT_TRUE(smulti::streamlabs_login_url(0, "x").empty());
	EXPECT_TRUE(smulti::streamlabs_login_url(65536, "x").empty());
	EXPECT_TRUE(smulti::streamlabs_login_url(80, "").empty());
}

SMULTI_TEST(streamlabs_login_callback_parses_code)
{
	const StreamlabsLoginCallback a = smulti::streamlabs_login_parse_callback(
		"GET /?success=true&code=FAKE-CODE-abc123 HTTP/1.1\r\nHost: 127.0.0.1:53123\r\n\r\n");
	EXPECT_EQ(a.kind, StreamlabsCallbackKind::Code);
	EXPECT_EQ(a.code, std::string(kFakeCode));

	/* any path, any parameter order, percent-encoding is decoded */
	const StreamlabsLoginCallback b = smulti::streamlabs_login_parse_callback(
		"GET /callback/x?state=1&code=FAKE%2DCODE%2Dabc123&success=true HTTP/1.1\r\n\r\n");
	EXPECT_EQ(b.kind, StreamlabsCallbackKind::Code);
	EXPECT_EQ(b.code, std::string(kFakeCode));

	/* the request line has not fully arrived yet */
	const StreamlabsLoginCallback partial = smulti::streamlabs_login_parse_callback("GET /?success=true&co");
	EXPECT_EQ(partial.kind, StreamlabsCallbackKind::Incomplete);
	EXPECT_TRUE(partial.code.empty());

	/* the browser's favicon request and other paths without callback parameters are not the callback */
	EXPECT_EQ(smulti::streamlabs_login_parse_callback("GET /favicon.ico HTTP/1.1\r\n\r\n").kind,
		  StreamlabsCallbackKind::NotCallback);
	EXPECT_EQ(smulti::streamlabs_login_parse_callback("GET /?foo=bar HTTP/1.1\r\n\r\n").kind,
		  StreamlabsCallbackKind::NotCallback);
	/* only GET is accepted */
	EXPECT_EQ(smulti::streamlabs_login_parse_callback("POST /?success=true&code=x HTTP/1.1\r\n\r\n").kind,
		  StreamlabsCallbackKind::NotCallback);
	/* garbage, and a request line longer than the cap */
	EXPECT_EQ(smulti::streamlabs_login_parse_callback("\x16\x03\x01 tls hello\r\n").kind,
		  StreamlabsCallbackKind::NotCallback);
	EXPECT_EQ(smulti::streamlabs_login_parse_callback("GET /?" + std::string(9000, 'a')).kind,
		  StreamlabsCallbackKind::NotCallback);
}

SMULTI_TEST(streamlabs_login_callback_rejects_failure)
{
	const char *failures[] = {
		"GET /?success=false&code=FAKE-CODE-abc123 HTTP/1.1\r\n\r\n", // Streamlabs says no
		"GET /?success=true HTTP/1.1\r\n\r\n",                         // no code
		"GET /?success=true&code= HTTP/1.1\r\n\r\n",                   // empty code
		"GET /?code=FAKE-CODE-abc123 HTTP/1.1\r\n\r\n",                // success missing
		"GET /?success=TRUE1&code=FAKE-CODE-abc123 HTTP/1.1\r\n\r\n",  // success not exactly true
		"GET /?success=true&code=FAKE%zzCODE HTTP/1.1\r\n\r\n",        // broken percent escape
		"GET /?success=true&code=FAKE%20CODE HTTP/1.1\r\n\r\n",        // a space is not a code character
	};
	for (const char *request : failures) {
		const StreamlabsLoginCallback r = smulti::streamlabs_login_parse_callback(request);
		EXPECT_EQ(r.kind, StreamlabsCallbackKind::Failed);
		EXPECT_TRUE(r.code.empty());
		EXPECT_TRUE(!r.error_message.empty());
		EXPECT_TRUE(!contains(r.error_message, "FAKE"));
	}
}

SMULTI_TEST(streamlabs_login_exchange_parses_token)
{
	const StreamlabsRequest req = smulti::streamlabs_login_exchange_request("00ff", "FAKE/CODE+1");
	EXPECT_EQ(req.method, std::string("GET"));
	EXPECT_EQ(req.url, std::string("https://streamlabs.com/api/v5/slobs/auth/data?code_verifier=00ff"
				       "&code=FAKE%2FCODE%2B1"));
	EXPECT_TRUE(req.form.empty());

	const StreamlabsTokenImport ok = smulti::streamlabs_login_parse_exchange(
		200, R"({"success":true,"data":{"oauth_token":"FAKE-OAUTH-TOKEN-not-real","other":1}})");
	EXPECT_TRUE(ok.ok);
	EXPECT_EQ(ok.token, std::string(kFakeOauth));
	EXPECT_TRUE(ok.error_message.empty());

	/* success must be JSON true; the token must be a usable string */
	const char *bad[] = {
		R"({"success":false,"data":{"oauth_token":"FAKE-OAUTH-TOKEN-not-real"}})",
		R"({"success":"true","data":{"oauth_token":"FAKE-OAUTH-TOKEN-not-real"}})",
		R"({"data":{"oauth_token":"FAKE-OAUTH-TOKEN-not-real"}})",
		R"({"success":true,"data":{}})",
		R"({"success":true,"data":{"oauth_token":""}})",
		R"({"success":true,"data":{"oauth_token":12345}})",
		R"({"success":true,"data":{"oauth_token":"FAKE OAUTH"}})",
		R"({"success":true,"oauth_token":"FAKE-OAUTH-TOKEN-not-real"})",
	};
	for (const char *body : bad) {
		const StreamlabsTokenImport r = smulti::streamlabs_login_parse_exchange(200, body);
		EXPECT_TRUE(!r.ok);
		EXPECT_TRUE(r.token.empty());
		EXPECT_TRUE(!r.error_message.empty());
	}
	const std::string too_long = std::string(R"({"success":true,"data":{"oauth_token":")") +
				     std::string(513, 'a') + "\"}}";
	EXPECT_TRUE(!smulti::streamlabs_login_parse_exchange(200, too_long).ok);
	/* only HTTP 200 counts */
	EXPECT_TRUE(!smulti::streamlabs_login_parse_exchange(
			     201, R"({"success":true,"data":{"oauth_token":"FAKE-OAUTH-TOKEN-not-real"}})")
			     .ok);
}

SMULTI_TEST(streamlabs_login_exchange_error_has_no_secrets)
{
	const std::string error_body =
		R"({"success":false,"message":"bad code FAKE-CODE-abc123 for verifier 00ff","data":{"oauth_token":"FAKE-OAUTH-TOKEN-not-real"}})";
	const int statuses[] = {200, 400, 401, 403, 500};
	for (int status : statuses) {
		const StreamlabsTokenImport r = smulti::streamlabs_login_parse_exchange(status, error_body);
		EXPECT_TRUE(!r.ok);
		EXPECT_TRUE(r.token.empty());
		EXPECT_TRUE(!r.error_message.empty());
		EXPECT_TRUE(!contains(r.error_message, kFakeCode));
		EXPECT_TRUE(!contains(r.error_message, kFakeOauth));
		EXPECT_TRUE(!contains(r.error_message, "bad code"));
		EXPECT_TRUE(!contains(r.error_message, "00ff"));
	}
	EXPECT_TRUE(contains(smulti::streamlabs_login_parse_exchange(500, error_body).error_message, "500"));

	/* truncated or hostile bodies are refused before libobs could log a fragment of the token */
	log_handler_t previous_handler = nullptr;
	void *previous_param = nullptr;
	base_get_log_handler(&previous_handler, &previous_param);
	g_login_log.clear();
	base_set_log_handler(capture_login_log, nullptr);
	const std::string hostile[] = {
		R"({"success":true,"data":{"oauth_token":"FAKE-OAUTH-TOK)",
		std::string(R"({"success":true,"data":{"oauth_token":"FAKE-OAUTH-TOK)") + "\xff" + R"("}})",
		R"({"success":true,"data":{"oauth_token":"FAKE-OAUTH-TOK\u0000"}})",
		R"({"success":true,"data":{"oauth_token":"FAKE-OAUTH-TOK","oauth_token":"x"}})",
	};
	for (const auto &body : hostile) {
		const StreamlabsTokenImport r = smulti::streamlabs_login_parse_exchange(200, body);
		EXPECT_TRUE(!r.ok);
		EXPECT_TRUE(!contains(r.error_message, "FAKE"));
	}
	base_set_log_handler(previous_handler, previous_param);
	EXPECT_TRUE(!contains(g_login_log, "FAKE"));

	/* callback errors never quote the code either */
	const StreamlabsLoginCallback cb =
		smulti::streamlabs_login_parse_callback("GET /?success=false&code=FAKE-CODE-abc123 HTTP/1.1\r\n\r\n");
	EXPECT_TRUE(!contains(cb.error_message, kFakeCode));
}

SMULTI_TEST(streamlabs_desktop_token_scan_last_match)
{
	/* a leveldb .log is binary: records with control bytes around the JSON text */
	std::string blob;
	blob += std::string("\x01\x00\x00\x2a", 4);
	blob += R"(_file://\x00\x01{"apiToken":"0123456789abcdef0123456789abcdef","other":1})";
	blob += std::string("\x00\xff\x13\x37", 4);
	blob += R"({"apiToken":"fedcba9876543210FEDCBA9876543210aa"})";
	blob += std::string("\x02\x00", 2);
	blob += R"({"apiToken":"not-hex-but-later-zzzzzzzzzzzzzz"})"; // not a match: skipped
	blob += std::string("\x03", 1);
	const StreamlabsTokenImport r = smulti::streamlabs_desktop_scan_token(blob);
	EXPECT_TRUE(r.ok);
	EXPECT_EQ(r.token, std::string("fedcba9876543210FEDCBA9876543210aa"));

	const StreamlabsTokenImport single =
		smulti::streamlabs_desktop_scan_token(R"(xx"apiToken":"0123456789abcdef"yy)");
	EXPECT_TRUE(single.ok);
	EXPECT_EQ(single.token, std::string("0123456789abcdef"));

	const StreamlabsTokenImport none = smulti::streamlabs_desktop_scan_token("no token in here");
	EXPECT_TRUE(!none.ok);
	EXPECT_TRUE(!none.error_message.empty());
	EXPECT_TRUE(!smulti::streamlabs_desktop_scan_token("").ok);
}

SMULTI_TEST(streamlabs_desktop_token_scan_rejects_non_hex)
{
	const char *bad[] = {
		R"({"apiToken":"FAKE-TOKEN-not-hex-0123456789"})", // non-hex characters
		R"({"apiToken":"0123456789abcde"})",               // 15 characters: too short
		R"({"apiToken":""})",                              // empty
		R"({"apiToken":"0123456789abcdef)",                // unterminated
		R"({"apiToken": "0123456789abcdef"})",             // not the stored form
		R"({"apiToken":"0123456789abcdef 0123456789"})",   // space inside
	};
	for (const char *text : bad) {
		const StreamlabsTokenImport r = smulti::streamlabs_desktop_scan_token(text);
		EXPECT_TRUE(!r.ok);
		EXPECT_TRUE(r.token.empty());
		EXPECT_TRUE(!contains(r.error_message, "FAKE"));
	}
	/* 128 hex characters is the longest accepted, 129 is refused */
	const std::string max_ok = std::string(R"("apiToken":")") + std::string(128, 'a') + "\"";
	const std::string too_long = std::string(R"("apiToken":")") + std::string(129, 'a') + "\"";
	EXPECT_TRUE(smulti::streamlabs_desktop_scan_token(max_ok).ok);
	EXPECT_TRUE(!smulti::streamlabs_desktop_scan_token(too_long).ok);
}
