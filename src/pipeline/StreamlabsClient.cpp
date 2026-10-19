/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "StreamlabsClient.hpp"
#include "StreamlabsTokenVault.hpp"
#include "../plugin-support.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>

namespace smulti {

namespace {

constexpr int kRequestTimeoutMs = 15000;
constexpr int kEndTimeoutMs = 5000;
constexpr size_t kMaxBodyBytes = 1024 * 1024;
constexpr const char *kSessionFileName = "streamlabs_tiktok_session.txt";

std::atomic<StreamlabsClient *> g_client{nullptr};

std::wstring to_wide(const std::string &utf8)
{
	if (utf8.empty())
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
	if (n > 0)
		MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], n);
	return out;
}

void wipe_wide(std::wstring &s)
{
	if (!s.empty())
		SecureZeroMemory(&s[0], s.size() * sizeof(wchar_t));
	s.clear();
}

/* A token copy that is wiped when the last job holding it is gone. */
std::shared_ptr<std::string> token_holder(std::string &&token)
{
	return std::shared_ptr<std::string>(new std::string(std::move(token)), [](std::string *s) {
		streamlabs_vault::wipe(*s);
		delete s;
	});
}

std::shared_ptr<std::string> load_token()
{
	std::string token;
	if (!streamlabs_vault::load(token))
		return nullptr;
	return token_holder(std::move(token));
}

std::string transport_text(DWORD code)
{
	switch (code) {
	case ERROR_WINHTTP_TIMEOUT:
		return "the request timed out";
	case ERROR_WINHTTP_NAME_NOT_RESOLVED:
		return "the host name could not be resolved";
	case ERROR_WINHTTP_CANNOT_CONNECT:
		return "the connection failed";
	case ERROR_WINHTTP_OPERATION_CANCELLED:
		return "the request was cancelled";
	case ERROR_WINHTTP_SECURE_FAILURE:
		return "the TLS connection failed";
	default:
		return "network error " + std::to_string(code);
	}
}

std::string random_boundary()
{
	std::random_device rd;
	char buf[48];
	std::snprintf(buf, sizeof(buf), "----StreamMulticast%08x%08x", rd(), rd());
	return buf;
}

/* ---- session id file (worker thread or after the worker is joined) ---- */

std::string session_file_path()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path(kSessionFileName);
	if (!path)
		return {};
	std::string out = path;
	bfree(path);
	return out;
}

std::string read_session_file()
{
	const std::string path = session_file_path();
	if (path.empty())
		return {};
	char *raw = os_quick_read_utf8_file(path.c_str());
	if (!raw)
		return {};
	std::string id = raw;
	bfree(raw);
	id.erase(std::remove_if(id.begin(), id.end(), [](char c) { return c == '\r' || c == '\n' || c == ' '; }),
		 id.end());
	return id;
}

void write_session_file(const std::string &id)
{
	const std::string path = session_file_path();
	if (path.empty() ||
	    !os_quick_write_utf8_file_safe(path.c_str(), id.c_str(), id.size(), false, "tmp", nullptr))
		obs_log(LOG_WARNING, "streamlabs tiktok: could not persist the session id");
}

void clear_session_file_if(const std::string &id)
{
	if (read_session_file() != id)
		return;
	const std::string path = session_file_path();
	if (!path.empty())
		os_unlink(path.c_str());
}

void log_ended(const StreamlabsResult &r)
{
	obs_log(LOG_INFO, "streamlabs tiktok: session ended (success=%s)", r.ok ? "true" : "false");
	if (!r.ok)
		obs_log(LOG_WARNING, "streamlabs tiktok: %s", r.error_message.c_str());
}

/* Close a WinHTTP handle and null it. */
void close_handle(HINTERNET &h)
{
	if (h)
		WinHttpCloseHandle(h);
	h = nullptr;
}

} // namespace

StreamlabsClient *streamlabs_client()
{
	return g_client.load();
}

void set_streamlabs_client(StreamlabsClient *client)
{
	g_client.store(client);
}

/* -----------------------------------------------------------------------
 * Lifetime
 * ----------------------------------------------------------------------- */
StreamlabsClient::StreamlabsClient(UiPost post_to_ui)
	: m_post(std::move(post_to_ui)),
	  m_alive(std::make_shared<std::atomic<bool>>(true))
{
	m_worker = std::thread([this]() { worker_loop(); });
}

StreamlabsClient::~StreamlabsClient()
{
	shutdown_and_end_blocking();
}

void StreamlabsClient::CancelSlot::cancel()
{
	std::lock_guard<std::mutex> lock(mutex);
	cancelled = true;
	if (request) {
		/* Closing the handle makes the blocked WinHTTP call on the worker return
		 * ERROR_WINHTTP_OPERATION_CANCELLED; the worker sees request == nullptr and
		 * does not close it a second time. */
		HINTERNET h = static_cast<HINTERNET>(request);
		request = nullptr;
		WinHttpCloseHandle(h);
	}
}

void StreamlabsClient::worker_loop()
{
	for (;;) {
		std::function<void()> job;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait(lock, [this]() { return m_stopping || !m_jobs.empty(); });
			if (m_stopping)
				return;
			job = std::move(m_jobs.front());
			m_jobs.pop_front();
		}
		job();
	}
}

void StreamlabsClient::enqueue(std::function<void()> job)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_stopping)
			return;
		m_jobs.push_back(std::move(job));
	}
	m_cv.notify_one();
}

void StreamlabsClient::post(std::function<void()> fn)
{
	auto alive = m_alive;
	m_post([alive, fn = std::move(fn)]() {
		if (alive->load())
			fn();
	});
}

void StreamlabsClient::shutdown_and_end_blocking()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_shut_down)
			return;
		m_shut_down = true;
		m_stopping = true;
		m_jobs.clear(); // a dropped end keeps its id in m_closing_ids
	}
	m_alive->store(false);
	m_cv.notify_all();
	m_worker_slot.cancel();
	if (m_worker.joinable())
		m_worker.join();

	std::vector<std::string> ids;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		ids = m_closing_ids;
		if (!m_open_id.empty())
			ids.push_back(m_open_id);
		m_closing_ids.clear();
		m_open_id.clear();
		m_open_owner.clear();
		m_open_name.clear();
	}
	if (ids.empty())
		return;
	auto token = load_token();
	if (!token) {
		obs_log(LOG_WARNING, "streamlabs tiktok: no token stored, open session cannot be ended");
		return;
	}
	for (const auto &id : ids)
		end_blocking_bounded(*token, id);
}

/* -----------------------------------------------------------------------
 * WinHTTP
 * ----------------------------------------------------------------------- */
StreamlabsClient::HttpResult StreamlabsClient::perform(const StreamlabsRequest &req, const std::string &token,
						       int timeout_ms, CancelSlot &slot)
{
	HttpResult res;
	std::wstring url = to_wide(req.url);

	URL_COMPONENTS uc = {};
	uc.dwStructSize = sizeof(uc);
	uc.dwHostNameLength = static_cast<DWORD>(-1);
	uc.dwUrlPathLength = static_cast<DWORD>(-1);
	uc.dwExtraInfoLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS || !uc.lpszHostName) {
		res.transport_error = "only https URLs are allowed";
		return res;
	}
	const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
	std::wstring path = uc.lpszUrlPath ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) : L"/";
	if (uc.lpszExtraInfo)
		path += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);

	const std::wstring agent = to_wide(kStreamlabsUserAgent);
	HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connect = nullptr;
	HINTERNET request = nullptr;
	DWORD error = 0;

	if (session) {
		WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
		DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
		protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
		if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
			DWORD tls12 = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
			WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls12, sizeof(tls12));
		}
		connect = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
	}
	if (connect)
		request = WinHttpOpenRequest(connect, to_wide(req.method).c_str(), path.c_str(), nullptr,
					     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!request) {
		error = GetLastError();
		close_handle(connect);
		close_handle(session);
		res.transport_error = transport_text(error);
		return res;
	}

	/* Register for cancellation; a cancel that already happened aborts right here. */
	{
		std::lock_guard<std::mutex> lock(slot.mutex);
		if (slot.cancelled) {
			close_handle(request);
			close_handle(connect);
			close_handle(session);
			res.transport_error = transport_text(ERROR_WINHTTP_OPERATION_CANCELLED);
			return res;
		}
		slot.request = request;
	}

	std::wstring headers = L"authorization: Bearer " + to_wide(token) + L"\r\n";
	std::string body;
	if (!req.form.empty()) {
		const std::string boundary = random_boundary();
		headers += L"Content-Type: multipart/form-data; boundary=" + to_wide(boundary) + L"\r\n";
		body = streamlabs_multipart_body(req.form, boundary);
	}

	bool ok = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L),
				     body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(body.data()),
				     static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
		  WinHttpReceiveResponse(request, nullptr);
	wipe_wide(headers);
	if (ok) {
		DWORD status = 0;
		DWORD size = sizeof(status);
		ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
		res.status = static_cast<int>(status);
	}
	while (ok) {
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(request, &available)) {
			ok = false;
			break;
		}
		if (available == 0)
			break;
		if (res.body.size() + available > kMaxBodyBytes) {
			ok = false;
			SetLastError(ERROR_WINHTTP_INVALID_SERVER_RESPONSE);
			break;
		}
		const size_t old = res.body.size();
		res.body.resize(old + available);
		DWORD read = 0;
		if (!WinHttpReadData(request, &res.body[old], available, &read)) {
			ok = false;
			break;
		}
		res.body.resize(old + read);
	}
	if (!ok)
		error = GetLastError();

	{
		std::lock_guard<std::mutex> lock(slot.mutex);
		if (slot.request == request) {
			slot.request = nullptr;
			close_handle(request); // not cancelled: ours to close
		}
		/* else: CancelSlot::cancel() already closed it */
	}
	close_handle(connect);
	close_handle(session);

	if (!ok) {
		streamlabs_vault::wipe(res.body);
		res.status = 0;
		res.transport_error = transport_text(error);
		return res;
	}
	res.transport_ok = true;
	return res;
}

StreamlabsResult StreamlabsClient::end_now(const std::string &token, const std::string &session_id, CancelSlot &slot)
{
	HttpResult http = perform(streamlabs_end_request(session_id), token, kEndTimeoutMs, slot);
	StreamlabsResult r = http.transport_ok ? streamlabs_parse_end(http.status, http.body)
					       : streamlabs_transport_error(http.transport_error);
	streamlabs_vault::wipe(http.body);
	log_ended(r);
	if (r.ok)
		clear_session_file_if(session_id);
	return r;
}

void StreamlabsClient::end_blocking_bounded(const std::string &token, const std::string &session_id)
{
	struct Shared {
		std::mutex mutex;
		std::condition_variable cv;
		bool done = false;
		CancelSlot slot;
	};
	auto shared = std::make_shared<Shared>();
	auto token_copy = token_holder(std::string(token));
	std::thread t([this, shared, token_copy, session_id]() {
		end_now(*token_copy, session_id, shared->slot);
		std::lock_guard<std::mutex> lock(shared->mutex);
		shared->done = true;
		shared->cv.notify_all();
	});
	{
		std::unique_lock<std::mutex> lock(shared->mutex);
		shared->cv.wait_for(lock, std::chrono::milliseconds(kEndTimeoutMs), [&]() { return shared->done; });
	}
	shared->slot.cancel(); // no-op when done; aborts a request still running after 5 s
	t.join();
}

/* -----------------------------------------------------------------------
 * Calls
 * ----------------------------------------------------------------------- */
void StreamlabsClient::info(ResultFn done)
{
	auto token = load_token();
	if (!token) {
		StreamlabsResult r;
		r.error_code = StreamlabsError::NoToken;
		r.error_message = "Not connected. Import a token from the generator config.";
		post([done, r]() { done(r); });
		return;
	}
	enqueue([this, token, done]() {
		HttpResult http = perform(streamlabs_info_request(), *token, kRequestTimeoutMs, m_worker_slot);
		StreamlabsResult r = http.transport_ok ? streamlabs_parse_info(http.status, http.body)
						       : streamlabs_transport_error(http.transport_error);
		streamlabs_vault::wipe(http.body);
		post([done, r]() { done(r); });
	});
}

void StreamlabsClient::search(const std::string &query, ResultFn done)
{
	auto token = load_token();
	if (!token) {
		StreamlabsResult r;
		r.error_code = StreamlabsError::NoToken;
		r.error_message = "Not connected. Import a token from the generator config.";
		post([done, r]() { done(r); });
		return;
	}
	enqueue([this, token, query, done]() {
		HttpResult http = perform(streamlabs_search_request(query), *token, kRequestTimeoutMs, m_worker_slot);
		StreamlabsResult r = http.transport_ok ? streamlabs_parse_search(http.status, http.body)
						       : streamlabs_transport_error(http.transport_error);
		streamlabs_vault::wipe(http.body);
		post([done, r]() { done(r); });
	});
}

void StreamlabsClient::start_session(const std::string &endpoint_id, const std::string &endpoint_name,
				     const std::string &title, const std::string &category_id, AdoptFn adopt)
{
	auto fail_now = [this, &adopt](const std::string &message, StreamlabsError code) {
		StreamlabsResult r;
		r.error_code = code;
		r.error_message = message;
		post([adopt, r]() { adopt(r); });
	};

	std::string other_owner_name;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_open_owner.empty() && m_open_owner != endpoint_id)
			other_owner_name = m_open_name;
	}
	if (!other_owner_name.empty()) {
		fail_now("The Streamlabs TikTok session is already used by '" + other_owner_name + "'.",
			 StreamlabsError::Rejected);
		return;
	}
	auto token = load_token();
	if (!token) {
		fail_now("Not connected to Streamlabs. Import a token in the endpoint settings.",
			 StreamlabsError::NoToken);
		return;
	}

	enqueue([this, token, endpoint_id, endpoint_name, title, category_id, adopt]() {
		/* 1. End what is still open for this endpoint, and a session left by a crash.
		 * Another endpoint that started meanwhile keeps its session. */
		std::vector<std::string> stale;
		std::string busy_with;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_open_id.empty() && m_open_owner != endpoint_id)
				busy_with = m_open_name.empty() ? std::string("another endpoint") : m_open_name;
			else if (!m_open_id.empty()) {
				stale.push_back(m_open_id);
				m_open_id.clear();
				m_open_owner.clear();
				m_open_name.clear();
			}
		}
		if (!busy_with.empty()) {
			StreamlabsResult busy;
			busy.error_code = StreamlabsError::Rejected;
			busy.error_message = "The Streamlabs TikTok session is already used by '" + busy_with + "'.";
			post([adopt, busy]() { adopt(busy); });
			return;
		}
		const std::string from_file = read_session_file();
		if (!from_file.empty() && std::find(stale.begin(), stale.end(), from_file) == stale.end())
			stale.push_back(from_file);
		for (const auto &id : stale) {
			obs_log(LOG_INFO, "streamlabs tiktok: ending a previous session before a new start");
			end_now(*token, id, m_worker_slot);
		}

		/* 2. Start. */
		HttpResult http =
			perform(streamlabs_start_request(title, category_id), *token, kRequestTimeoutMs, m_worker_slot);
		StreamlabsResult r = http.transport_ok ? streamlabs_parse_start(http.status, http.body)
						       : streamlabs_transport_error(http.transport_error);
		streamlabs_vault::wipe(http.body);

		if (r.ok) {
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_open_id = r.id;
				m_open_owner = endpoint_id;
				m_open_name = endpoint_name;
			}
			write_session_file(r.id);
			obs_log(LOG_INFO, "streamlabs tiktok: session started for '%s'", endpoint_name.c_str());
		} else {
			obs_log(LOG_WARNING, "streamlabs tiktok: start failed for '%s': %s", endpoint_name.c_str(),
				r.error_message.c_str());
		}

		/* 3. Hand over on the UI thread; an unclaimed session is ended again. */
		post([this, adopt, r, endpoint_id]() mutable {
			const bool adopted = adopt(r);
			const bool started = r.ok;
			streamlabs_vault::wipe(r.key);
			streamlabs_vault::wipe(r.rtmp);
			if (started && !adopted)
				end_session(endpoint_id);
		});
	});
}

void StreamlabsClient::end_session(const std::string &endpoint_id)
{
	std::string id;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_shut_down || m_open_id.empty() || m_open_owner != endpoint_id)
			return;
		id = m_open_id;
		m_open_id.clear();
		m_open_owner.clear();
		m_open_name.clear();
		m_closing_ids.push_back(id);
	}
	auto token = load_token();
	if (!token) {
		obs_log(LOG_WARNING, "streamlabs tiktok: no token stored, session cannot be ended");
		std::lock_guard<std::mutex> lock(m_mutex);
		m_closing_ids.erase(std::remove(m_closing_ids.begin(), m_closing_ids.end(), id), m_closing_ids.end());
		return; // the id stays in the session file and is ended before the next start
	}
	enqueue([this, token, id]() {
		end_now(*token, id, m_worker_slot);
		std::lock_guard<std::mutex> lock(m_mutex);
		m_closing_ids.erase(std::remove(m_closing_ids.begin(), m_closing_ids.end(), id), m_closing_ids.end());
	});
}

} // namespace smulti
