// Streamlabs TikTok provider: pure request building, response parsing, token import. No network, obs_data only.
// Every token, key and URL below is a fake fixture value.
#include "TestRunner.hpp"
#include "core/Endpoint.hpp"
#include "core/StreamlabsTikTok.hpp"

#include <obs-data.h>
#include <util/base.h>

#include <cstdarg>
#include <cstdio>
#include <string>

using smulti::Endpoint;
using smulti::StreamlabsError;
using smulti::StreamlabsRequest;
using smulti::StreamlabsResult;
using smulti::StreamlabsTokenImport;

namespace {

bool contains(const std::string &haystack, const std::string &needle)
{
	return haystack.find(needle) != std::string::npos;
}

std::string g_captured_log;

void capture_log(int, const char *format, va_list args, void *)
{
	char buf[1024];
	std::vsnprintf(buf, sizeof(buf), format, args);
	g_captured_log += buf;
	g_captured_log += '\n';
}

const char *kFakeKey = "FAKE-STREAM-KEY-123";
const char *kFakeToken = "FAKE-TOKEN-not-a-real-one";

} // namespace

SMULTI_TEST(streamlabs_tiktok_parse_start_ok)
{
	StreamlabsResult r = smulti::streamlabs_parse_start(
		200, R"({"id":"987654","rtmp":"rtmp://push.example.test/live/","key":"FAKE-STREAM-KEY-123"})");
	EXPECT_TRUE(r.ok);
	EXPECT_EQ(r.error_code, StreamlabsError::None);
	EXPECT_EQ(r.id, "987654");
	EXPECT_EQ(r.rtmp, "rtmp://push.example.test/live/");
	EXPECT_EQ(r.key, kFakeKey);

	/* a numeric id is accepted and kept as its decimal text */
	StreamlabsResult n = smulti::streamlabs_parse_start(
		200, R"({"id":123456789012,"rtmp":"rtmp://push.example.test/live/","key":"k"})");
	EXPECT_TRUE(n.ok);
	EXPECT_EQ(n.id, "123456789012");
}

SMULTI_TEST(streamlabs_tiktok_parse_start_missing_key_is_error)
{
	StreamlabsResult no_key =
		smulti::streamlabs_parse_start(200, R"({"id":"1","rtmp":"rtmp://push.example.test/live/"})");
	EXPECT_TRUE(!no_key.ok);
	EXPECT_EQ(no_key.error_code, StreamlabsError::MissingField);
	EXPECT_TRUE(no_key.key.empty());

	StreamlabsResult empty_key =
		smulti::streamlabs_parse_start(200, R"({"id":"1","rtmp":"rtmp://push.example.test/live/","key":""})");
	EXPECT_TRUE(!empty_key.ok);

	StreamlabsResult no_rtmp = smulti::streamlabs_parse_start(200, R"({"id":"1","key":"k"})");
	EXPECT_TRUE(!no_rtmp.ok);

	StreamlabsResult no_id = smulti::streamlabs_parse_start(200, R"({"rtmp":"rtmp://x.test/a","key":"k"})");
	EXPECT_TRUE(!no_id.ok);
	/* a failed parse never hands out partial connection data */
	EXPECT_TRUE(no_id.rtmp.empty());
	EXPECT_TRUE(no_id.key.empty());

	StreamlabsResult not_json = smulti::streamlabs_parse_start(200, "<html>oops</html>");
	EXPECT_TRUE(!not_json.ok);
	EXPECT_EQ(not_json.error_code, StreamlabsError::BadResponse);
}

SMULTI_TEST(streamlabs_tiktok_parse_start_error_message_has_no_secrets)
{
	const std::string missing_id_body =
		R"({"rtmp":"rtmp://push.example.test/live/?sig=SECRETSIG","key":"FAKE-STREAM-KEY-123"})";
	StreamlabsResult a = smulti::streamlabs_parse_start(200, missing_id_body);
	EXPECT_TRUE(!a.ok);
	EXPECT_TRUE(!a.error_message.empty());
	EXPECT_TRUE(!contains(a.error_message, kFakeKey));
	EXPECT_TRUE(!contains(a.error_message, "SECRETSIG"));
	EXPECT_TRUE(!contains(a.error_message, "push.example.test"));

	const std::string error_body = R"({"message":"bad token FAKE-TOKEN-not-a-real-one","key":"FAKE-STREAM-KEY-123"})";
	StreamlabsResult b = smulti::streamlabs_parse_start(500, error_body);
	EXPECT_TRUE(!b.ok);
	EXPECT_EQ(b.error_code, StreamlabsError::Http);
	EXPECT_TRUE(contains(b.error_message, "500"));
	EXPECT_TRUE(!contains(b.error_message, kFakeToken));
	EXPECT_TRUE(!contains(b.error_message, kFakeKey));
	EXPECT_TRUE(!contains(b.error_message, "bad token"));

	StreamlabsResult c = smulti::streamlabs_parse_start(401, error_body);
	EXPECT_EQ(c.error_code, StreamlabsError::Unauthorized);
	EXPECT_TRUE(!contains(c.error_message, kFakeToken));

	/* a truncated body (connection dropped mid-key) is rejected without a parser log of the fragment */
	log_handler_t previous_handler = nullptr;
	void *previous_param = nullptr;
	base_get_log_handler(&previous_handler, &previous_param);
	g_captured_log.clear();
	base_set_log_handler(capture_log, nullptr);
	StreamlabsResult d = smulti::streamlabs_parse_start(200, R"({"id":"1","rtmp":"rtmp://x.test/a","key":"FAKE-STR)");
	StreamlabsResult e = smulti::streamlabs_parse_start(200, R"({"key":"FAKE-STREAM-KEY-123",})");
	/* shapes that pass a plain syntax check but make jansson log "near '<token>'" (fix round 1) */
	const std::string hostile[] = {
		std::string(R"({"id":"1","rtmp":"rtmp://x.test/a","key":"FAKE-STR)") + "\xff\xfe" + R"("})", // invalid UTF-8
		std::string(R"({"id":"1","rtmp":"rtmp://x.test/a","key":"FAKE-STR)") + "\xc0\xaf" + R"("})", // overlong
		std::string(R"({"id":"1","rtmp":"rtmp://x.test/a","key":"FAKE-STR)") + "\xed\xa0\x80" + R"("})", // UTF-8 surrogate
		R"({"id":"1","rtmp":"rtmp://x.test/a","key":"FAKE-STR\u0000X"})",                 // \u0000
		R"({"id":"1","rtmp":"rtmp://x.test/a","key":"FAKE-STR\ud800X"})",                 // lone high surrogate
		R"({"id":"1","rtmp":"rtmp://x.test/a","key":"FAKE-STR\udc00X"})",                 // lone low surrogate
		R"({"id":99999999999999999999,"rtmp":"rtmp://x.test/a","key":"FAKE-STR"})",       // integer overflow
		R"({"id":-9223372036854775809,"rtmp":"rtmp://x.test/a","key":"FAKE-STR"})",       // integer underflow
		R"({"id":1e999,"rtmp":"rtmp://x.test/a","key":"FAKE-STR"})",                      // real overflow
		R"({"key":"FAKE-STR","key":"FAKE-STR2"})",                                        // duplicate key
		std::string(R"({"key":"FAKE-STR)") + std::string(1, '\0') + R"("})",              // embedded NUL
	};
	for (const auto &body : hostile) {
		StreamlabsResult h = smulti::streamlabs_parse_start(200, body);
		EXPECT_TRUE(!h.ok);
		EXPECT_EQ(h.error_code, StreamlabsError::BadResponse);
		EXPECT_TRUE(!smulti::streamlabs_json_is_object(body));
	}
	base_set_log_handler(previous_handler, previous_param);
	/* the hardened check still accepts valid text: UTF-8, escapes, a surrogate pair, int64 limits */
	EXPECT_TRUE(smulti::streamlabs_json_is_object(
		"{\"u\":\"\xc3\xa4\xe2\x82\xac\xf0\x9f\x98\x80\",\"e\":\"\\u00e4\\ud83d\\ude00\\n\","
		"\"a\":9223372036854775807,\"b\":-9223372036854775808,\"c\":1.5e10,\"d\":[1,{\"x\":null}]}"));
	EXPECT_TRUE(!smulti::streamlabs_json_is_object(R"({"a":9223372036854775808})"));
	EXPECT_TRUE(!d.ok);
	EXPECT_EQ(d.error_code, StreamlabsError::BadResponse);
	EXPECT_TRUE(!contains(d.error_message, "FAKE-STR"));
	EXPECT_TRUE(!e.ok);
	EXPECT_TRUE(!contains(g_captured_log, "FAKE-STR"));

	/* redaction helper used before any log line that could carry a secret */
	const std::string redacted = smulti::streamlabs_redact("token=FAKE-TOKEN-not-a-real-one key=FAKE-STREAM-KEY-123",
								  {kFakeToken, kFakeKey, ""});
	EXPECT_TRUE(!contains(redacted, kFakeToken));
	EXPECT_TRUE(!contains(redacted, kFakeKey));
	EXPECT_TRUE(contains(redacted, "token=***"));
}

SMULTI_TEST(streamlabs_tiktok_parse_info_can_be_live)
{
	StreamlabsResult r = smulti::streamlabs_parse_info(
		200, R"({"user":{"username":"fixture_user"},"application_status":{"status":"approved"},"can_be_live":true})");
	EXPECT_TRUE(r.ok);
	EXPECT_EQ(r.username, "fixture_user");
	EXPECT_EQ(r.application_status, "approved");
	EXPECT_EQ(r.can_be_live, true);

	StreamlabsResult no = smulti::streamlabs_parse_info(
		200, R"({"user":{"username":"fixture_user"},"application_status":{"status":"never_applied"},"can_be_live":false})");
	EXPECT_TRUE(no.ok);
	EXPECT_EQ(no.can_be_live, false);

	/* missing flag means "cannot go live", never true */
	StreamlabsResult missing = smulti::streamlabs_parse_info(200, R"({"user":{"username":"u"}})");
	EXPECT_TRUE(missing.ok);
	EXPECT_EQ(missing.can_be_live, false);

	StreamlabsResult unauthorized = smulti::streamlabs_parse_info(401, "{}");
	EXPECT_TRUE(!unauthorized.ok);
	EXPECT_EQ(unauthorized.error_code, StreamlabsError::Unauthorized);
}

SMULTI_TEST(streamlabs_tiktok_parse_end_success)
{
	StreamlabsResult ok = smulti::streamlabs_parse_end(200, R"({"success":true})");
	EXPECT_TRUE(ok.ok);
	EXPECT_EQ(ok.success, true);

	StreamlabsResult refused = smulti::streamlabs_parse_end(200, R"({"success":false})");
	EXPECT_TRUE(!refused.ok);
	EXPECT_EQ(refused.success, false);
	EXPECT_EQ(refused.error_code, StreamlabsError::Rejected);

	StreamlabsResult http = smulti::streamlabs_parse_end(404, R"({"success":true})");
	EXPECT_TRUE(!http.ok);
	EXPECT_EQ(http.success, false);
}

SMULTI_TEST(streamlabs_tiktok_search_appends_other)
{
	StreamlabsResult r = smulti::streamlabs_parse_search(
		200, R"({"categories":[{"full_name":"Just Chatting","game_mask_id":"111"},{"full_name":"Minecraft","game_mask_id":222}]})");
	EXPECT_TRUE(r.ok);
	EXPECT_EQ(r.categories.size(), static_cast<size_t>(3));
	if (r.categories.size() == 3) {
		EXPECT_EQ(r.categories[0].full_name, "Just Chatting");
		EXPECT_EQ(r.categories[0].game_mask_id, "111");
		EXPECT_EQ(r.categories[1].game_mask_id, "222");
		EXPECT_EQ(r.categories[2].full_name, "Other");
		EXPECT_EQ(r.categories[2].game_mask_id, "");
	}

	/* no categories at all still offers "Other" */
	StreamlabsResult empty = smulti::streamlabs_parse_search(200, R"({"categories":[]})");
	EXPECT_TRUE(empty.ok);
	EXPECT_EQ(empty.categories.size(), static_cast<size_t>(1));
	if (!empty.categories.empty())
		EXPECT_EQ(empty.categories.back().full_name, "Other");
}

SMULTI_TEST(streamlabs_tiktok_search_truncates_25)
{
	EXPECT_EQ(smulti::streamlabs_truncate_query("abcdefghijklmnopqrstuvwxyz0123"), "abcdefghijklmnopqrstuvwxy");
	EXPECT_EQ(smulti::streamlabs_truncate_query("short"), "short");
	/* 25 code points, not 25 bytes: "\xc3\xa4" is one character */
	std::string umlauts;
	for (int i = 0; i < 30; ++i)
		umlauts += "\xc3\xa4";
	const std::string cut = smulti::streamlabs_truncate_query(umlauts);
	EXPECT_EQ(cut.size(), static_cast<size_t>(50));

	StreamlabsRequest req = smulti::streamlabs_search_request("Just Chatting & more stuff here");
	EXPECT_EQ(req.method, "GET");
	EXPECT_EQ(req.url, "https://streamlabs.com/api/v5/slobs/tiktok/info?category=Just%20Chatting%20%26%20more%20stuf");
	EXPECT_TRUE(req.form.empty());
}

SMULTI_TEST(streamlabs_tiktok_import_generator_config_token)
{
	StreamlabsTokenImport file =
		smulti::streamlabs_import_generator_config_file(SMULTI_TEST_FIXTURE_DIR "/streamlabs_generator_config.json");
	EXPECT_TRUE(file.ok);
	EXPECT_EQ(file.token, kFakeToken);
	EXPECT_TRUE(file.error_message.empty());

	StreamlabsTokenImport text = smulti::streamlabs_import_generator_config(R"({"token":"  FAKE-TOKEN-xyz  "})");
	EXPECT_TRUE(text.ok);
	EXPECT_EQ(text.token, "FAKE-TOKEN-xyz");

	StreamlabsTokenImport missing_file =
		smulti::streamlabs_import_generator_config_file(SMULTI_TEST_FIXTURE_DIR "/does_not_exist.json");
	EXPECT_TRUE(!missing_file.ok);
	EXPECT_TRUE(missing_file.token.empty());
}

SMULTI_TEST(streamlabs_tiktok_import_rejects_empty_token)
{
	EXPECT_TRUE(!smulti::streamlabs_import_generator_config(R"({"token":""})").ok);
	EXPECT_TRUE(!smulti::streamlabs_import_generator_config(R"({"token":"   "})").ok);
	EXPECT_TRUE(!smulti::streamlabs_import_generator_config(R"({"title":"no token"})").ok);
	EXPECT_TRUE(!smulti::streamlabs_import_generator_config(R"({"token":123})").ok);
	EXPECT_TRUE(!smulti::streamlabs_import_generator_config("not json").ok);
	/* non-printable characters and over-long tokens are refused */
	EXPECT_TRUE(!smulti::streamlabs_import_generator_config(R"({"token":"FAKE\nTOKEN"})").ok);
	EXPECT_TRUE(!smulti::streamlabs_import_generator_config(std::string(R"({"token":")") + std::string(513, 'a') + "\"}").ok);
	EXPECT_TRUE(smulti::streamlabs_import_generator_config(std::string(R"({"token":")") + std::string(512, 'a') + "\"}").ok);

	StreamlabsTokenImport bad = smulti::streamlabs_import_generator_config(R"({"token":"FAKE\tTOKEN-SECRET"})");
	EXPECT_TRUE(bad.token.empty());
	EXPECT_TRUE(!contains(bad.error_message, "SECRET"));
}

SMULTI_TEST(streamlabs_tiktok_start_request_fields)
{
	StreamlabsRequest req = smulti::streamlabs_start_request("My Title", "222");
	EXPECT_EQ(req.method, "POST");
	EXPECT_EQ(req.url, "https://streamlabs.com/api/v5/slobs/tiktok/stream/start");
	EXPECT_EQ(req.form.size(), static_cast<size_t>(4));
	if (req.form.size() == 4) {
		EXPECT_EQ(req.form[0].name, "title");
		EXPECT_EQ(req.form[0].value, "My Title");
		EXPECT_EQ(req.form[1].name, "device_platform");
		EXPECT_EQ(req.form[1].value, "win32");
		EXPECT_EQ(req.form[2].name, "category");
		EXPECT_EQ(req.form[2].value, "222");
		EXPECT_EQ(req.form[3].name, "audience_type");
		EXPECT_EQ(req.form[3].value, "0");
	}

	/* "Other" sends an empty category */
	StreamlabsRequest other = smulti::streamlabs_start_request("t", "");
	EXPECT_EQ(other.form.size(), static_cast<size_t>(4));
	if (other.form.size() == 4)
		EXPECT_EQ(other.form[2].value, "");

	const std::string body = smulti::streamlabs_multipart_body(req.form, "BOUNDARY");
	EXPECT_TRUE(contains(body, "--BOUNDARY\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nMy Title\r\n"));
	EXPECT_TRUE(contains(body, "name=\"audience_type\"\r\n\r\n0\r\n--BOUNDARY--\r\n"));

	StreamlabsRequest info = smulti::streamlabs_info_request();
	EXPECT_EQ(info.method, "GET");
	EXPECT_EQ(info.url, "https://streamlabs.com/api/v5/slobs/tiktok/info");

	StreamlabsRequest end = smulti::streamlabs_end_request("987654");
	EXPECT_EQ(end.method, "POST");
	EXPECT_EQ(end.url, "https://streamlabs.com/api/v5/slobs/tiktok/stream/987654/end");
	EXPECT_TRUE(end.form.empty());

	EXPECT_TRUE(contains(smulti::kStreamlabsUserAgent, "StreamlabsDesktop/"));
	EXPECT_TRUE(contains(smulti::kStreamlabsUserAgent, "Electron/"));
}

SMULTI_TEST(streamlabs_tiktok_endpoint_key_provider_roundtrip)
{
	Endpoint ep = Endpoint::make_default("Tiktok");
	EXPECT_EQ(ep.key_provider, "");
	ep.key_provider = smulti::kKeyProviderStreamlabsTikTok;
	ep.streamlabs_title = "Fixture title";
	ep.streamlabs_category_id = "222";
	ep.streamlabs_category_name = "Minecraft";

	obs_data_t *data = ep.serialize();
	Endpoint out = Endpoint::deserialize(data);
	obs_data_release(data);
	EXPECT_EQ(out.key_provider, "streamlabs_tiktok");
	EXPECT_EQ(out.streamlabs_title, "Fixture title");
	EXPECT_EQ(out.streamlabs_category_id, "222");
	EXPECT_EQ(out.streamlabs_category_name, "Minecraft");

	/* a config without the keys (written by an older DLL) is a manual endpoint */
	obs_data_t *old = obs_data_create();
	obs_data_set_string(old, "id", "a-b-c");
	Endpoint legacy = Endpoint::deserialize(old);
	obs_data_release(old);
	EXPECT_EQ(legacy.key_provider, "");
	EXPECT_EQ(legacy.streamlabs_title, "");

	/* an unknown provider value falls back to manual */
	obs_data_t *unknown = obs_data_create();
	obs_data_set_string(unknown, "id", "a-b-c");
	obs_data_set_string(unknown, "key_provider", "something_else");
	Endpoint u = Endpoint::deserialize(unknown);
	obs_data_release(unknown);
	EXPECT_EQ(u.key_provider, "");
}

/* ---- session bookkeeping (fix round 1): which Streamlabs session id is ended, and when ---- */

using smulti::StreamlabsSessionBook;
using EndOutcome = smulti::StreamlabsSessionBook::EndOutcome;

SMULTI_TEST(streamlabs_tiktok_session_unadopted_ends_only_that_id)
{
	/* Stop + Start while fetch #1 is on the wire; job #2 runs before adopt1 reaches the UI thread. */
	StreamlabsSessionBook book;
	book.on_started("S1", "ep", "Tiktok");                 // job #1 reply
	auto claim = book.claim_for_start("ep");                // job #2: S1 is stale
	EXPECT_TRUE(claim.busy_with.empty());
	EXPECT_EQ(claim.stale.size(), static_cast<size_t>(1));
	EXPECT_TRUE(book.begin_end("S1"));
	book.finish_end("S1", EndOutcome::Ended);
	book.on_started("S2", "ep", "Tiktok");                 // job #2 reply
	EXPECT_TRUE(!book.release_unadopted("S1"));             // adopt1 refused: must not touch S2
	EXPECT_EQ(book.open_id(), "S2");

	/* Other order: adopt1 is refused first, then job #2 finds S1 again (session file). */
	StreamlabsSessionBook other;
	other.on_started("S1", "ep", "Tiktok");
	EXPECT_TRUE(other.release_unadopted("S1"));             // queues the one end of S1
	EXPECT_EQ(other.open_id(), "");
	auto claim2 = other.claim_for_start("ep");
	EXPECT_TRUE(claim2.stale.empty());
	EXPECT_TRUE(!other.begin_end("S1"));                    // stale-from-file: already being ended
}

SMULTI_TEST(streamlabs_tiktok_session_never_ends_twice)
{
	StreamlabsSessionBook book;
	book.on_started("S1", "ep", "Tiktok");
	EXPECT_EQ(book.release_owner("ep"), "S1");
	EXPECT_EQ(book.release_owner("ep"), "");                // second stop: nothing to end
	EXPECT_TRUE(!book.begin_end("S1"));                     // queued end owns it
	book.finish_end("S1", EndOutcome::Ended);
	EXPECT_TRUE(!book.begin_end("S1"));                     // ended once, never again
	EXPECT_TRUE(!book.release_unadopted("S1"));
	EXPECT_TRUE(book.take_for_shutdown().empty());

	/* a failed (not cancelled) end may be retried later, e.g. from the session file */
	StreamlabsSessionBook failed;
	failed.on_started("S9", "ep", "Tiktok");
	EXPECT_EQ(failed.release_owner("ep"), "S9");
	failed.finish_end("S9", EndOutcome::Failed);
	EXPECT_TRUE(failed.begin_end("S9"));
}

SMULTI_TEST(streamlabs_tiktok_session_cancelled_end_retried_at_shutdown)
{
	/* EXIT: stop() queued the end, the worker picked it up, shutdown cut it off. */
	StreamlabsSessionBook book;
	book.on_started("S1", "ep", "Tiktok");
	EXPECT_EQ(book.release_owner("ep"), "S1");
	book.finish_end("S1", EndOutcome::Cancelled);
	auto ids = book.take_for_shutdown();
	EXPECT_EQ(ids.size(), static_cast<size_t>(1));
	if (!ids.empty())
		EXPECT_EQ(ids[0], "S1");

	/* a queued end that was dropped with the queue is ended at shutdown as well */
	StreamlabsSessionBook dropped;
	dropped.on_started("S2", "ep", "Tiktok");
	EXPECT_EQ(dropped.release_owner("ep"), "S2");
	auto ids2 = dropped.take_for_shutdown();
	EXPECT_EQ(ids2.size(), static_cast<size_t>(1));
	EXPECT_TRUE(dropped.take_for_shutdown().empty());       // handed out once
}

SMULTI_TEST(streamlabs_tiktok_session_start_reply_during_shutdown_is_ended)
{
	/* the start reply arrives while shutdown waits for the worker: nobody adopts it */
	StreamlabsSessionBook book;
	book.on_started("S7", "ep", "Tiktok");
	auto ids = book.take_for_shutdown();
	EXPECT_EQ(ids.size(), static_cast<size_t>(1));
	if (!ids.empty())
		EXPECT_EQ(ids[0], "S7");
	EXPECT_EQ(book.open_id(), "");
}

SMULTI_TEST(streamlabs_tiktok_session_busy_for_other_endpoint)
{
	StreamlabsSessionBook book;
	book.on_started("S1", "ep-a", "Tiktok A");
	EXPECT_EQ(book.busy_with("ep-b"), "Tiktok A");
	EXPECT_EQ(book.busy_with("ep-a"), "");
	auto claim = book.claim_for_start("ep-b");
	EXPECT_EQ(claim.busy_with, "Tiktok A");
	EXPECT_TRUE(claim.stale.empty());
	EXPECT_EQ(book.open_id(), "S1");                        // the other endpoint keeps its session
	EXPECT_EQ(book.release_owner("ep-b"), "");
}
