/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

/* Winsock before anything that may pull in windows.h (winsock.h clash). */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include "StreamlabsLoginFlow.hpp"
#include "StreamlabsClient.hpp"
#include "StreamlabsTokenVault.hpp"
#include "../core/StreamlabsLogin.hpp"
#include "../plugin-support.h"

#include <obs-module.h>

#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

namespace smulti {

namespace {

constexpr int kPollMs = 250;                      ///< cancel / timeout check interval
constexpr int kConnectionReadMs = 5000;           ///< per-connection read limit
constexpr int64_t kDesktopLogMaxBytes = 64ll * 1024 * 1024;

std::mutex g_flows_mutex;
std::vector<StreamlabsLoginFlow *> g_flows; ///< live flows, for shutdown_all()

void wipe(std::string &s)
{
	if (!s.empty())
		SecureZeroMemory(&s[0], s.size());
	s.clear();
}

const char *kPageOk = "<!doctype html><html><head><meta charset=\"utf-8\"><title>StreamMulticast</title></head>"
		      "<body style=\"font-family:sans-serif\"><h2>Logged in to Streamlabs.</h2>"
		      "<p>You can close this tab and return to OBS.</p></body></html>";
const char *kPageFailed = "<!doctype html><html><head><meta charset=\"utf-8\"><title>StreamMulticast</title></head>"
			  "<body style=\"font-family:sans-serif\"><h2>The Streamlabs login did not complete.</h2>"
			  "<p>You can close this tab and try again in OBS.</p></body></html>";

void send_all(SOCKET s, const std::string &data)
{
	size_t sent = 0;
	while (sent < data.size()) {
		const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
		if (n <= 0)
			return;
		sent += static_cast<size_t>(n);
	}
}

void send_page(SOCKET s, const char *html)
{
	const std::string body = html;
	send_all(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
		    "Connection: close\r\nContent-Length: " +
			    std::to_string(body.size()) + "\r\n\r\n" + body);
}

void send_not_found(SOCKET s)
{
	send_all(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

/* Wait until `s` is readable, at most `ms`. 1 readable, 0 timeout, -1 error. */
int wait_readable(SOCKET s, int ms)
{
	fd_set set;
	FD_ZERO(&set);
	FD_SET(s, &set);
	timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	const int r = select(0, &set, nullptr, nullptr, &tv);
	return r == SOCKET_ERROR ? -1 : (r > 0 ? 1 : 0);
}

/* Read one request from `c` until the parser decides, the 8 KiB cap, 5 s, or cancel. */
StreamlabsLoginCallback read_callback(SOCKET c, const std::atomic<bool> &cancel)
{
	std::string request;
	StreamlabsLoginCallback cb;
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectionReadMs);
	char buf[2048];
	while (!cancel.load() && std::chrono::steady_clock::now() < until &&
	       request.size() < kStreamlabsCallbackMaxBytes) {
		const int ready = wait_readable(c, kPollMs);
		if (ready < 0)
			break;
		if (ready == 0)
			continue;
		const size_t room = kStreamlabsCallbackMaxBytes - request.size();
		const int n = recv(c, buf, static_cast<int>(std::min(room, sizeof(buf))), 0);
		if (n <= 0)
			break;
		request.append(buf, static_cast<size_t>(n));
		cb = streamlabs_login_parse_callback(request);
		if (cb.kind != StreamlabsCallbackKind::Incomplete)
			break;
	}
	SecureZeroMemory(buf, sizeof(buf));
	if (cb.kind == StreamlabsCallbackKind::Incomplete)
		cb = streamlabs_login_parse_callback(request); // cap reached or connection ended
	wipe(request);
	return cb;
}

} // namespace

/* What the listener thread hands to the UI thread. */
struct StreamlabsLoginFlow::Outcome {
	bool has_code = false;
	std::string code;    ///< secret
	std::string message; ///< sanitized error when !has_code
	~Outcome() { wipe(code); }
};

StreamlabsLoginFlow::StreamlabsLoginFlow(UiPost post) : m_post(std::move(post))
{
	std::lock_guard<std::mutex> lock(g_flows_mutex);
	g_flows.push_back(this);
}

StreamlabsLoginFlow::~StreamlabsLoginFlow()
{
	cancel();
	std::lock_guard<std::mutex> lock(g_flows_mutex);
	g_flows.erase(std::remove(g_flows.begin(), g_flows.end(), this), g_flows.end());
}

void StreamlabsLoginFlow::shutdown_all()
{
	std::vector<StreamlabsLoginFlow *> flows;
	{
		std::lock_guard<std::mutex> lock(g_flows_mutex);
		flows = g_flows;
	}
	for (auto *flow : flows)
		flow->cancel();
}

void StreamlabsLoginFlow::join_thread()
{
	if (m_thread.joinable())
		m_thread.join();
}

void StreamlabsLoginFlow::cancel()
{
	if (m_alive)
		m_alive->store(false);
	if (m_cancel)
		m_cancel->store(true);
	join_thread(); // the listener sees the flag within one poll tick and closes its socket
	wipe(m_verifier);
	if (m_running)
		obs_log(LOG_INFO, "streamlabs login: cancelled");
	m_running = false;
	m_status = nullptr;
	m_done = nullptr;
}

bool StreamlabsLoginFlow::start(StatusFn status, DoneFn done, std::string &error)
{
	if (m_running) {
		error = "A Streamlabs login is already waiting.";
		return false;
	}
	cancel(); // joins a finished listener of an earlier run
	if (!streamlabs_client()) {
		error = "Streamlabs is not available.";
		return false;
	}

	unsigned char random[kStreamlabsPkceRandomBytes];
	const NTSTATUS rng = BCryptGenRandom(nullptr, random, static_cast<ULONG>(sizeof(random)),
					     BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	if (!BCRYPT_SUCCESS(rng)) {
		SecureZeroMemory(random, sizeof(random));
		error = "Could not create the login secret (no system random source).";
		return false;
	}
	StreamlabsPkce pkce = streamlabs_login_pkce(random, sizeof(random));
	SecureZeroMemory(random, sizeof(random));

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		wipe(pkce.verifier);
		error = "Could not open the local login listener (Winsock).";
		return false;
	}
	SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only, never INADDR_ANY
	addr.sin_port = 0;                             // OS-assigned port
	int port = 0;
	if (listener != INVALID_SOCKET) {
		BOOL exclusive = TRUE;
		setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive),
			   sizeof(exclusive));
		int len = sizeof(addr);
		if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 && listen(listener, 4) == 0 &&
		    getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &len) == 0)
			port = ntohs(addr.sin_port);
	}
	const std::string url = streamlabs_login_url(port, pkce.challenge);
	if (url.empty()) {
		if (listener != INVALID_SOCKET)
			closesocket(listener);
		WSACleanup();
		wipe(pkce.verifier);
		error = "Could not open the local login listener.";
		return false;
	}

	if (!QDesktopServices::openUrl(QUrl(QString::fromStdString(url)))) {
		closesocket(listener);
		WSACleanup();
		wipe(pkce.verifier);
		error = "Could not open the web browser for the Streamlabs login.";
		return false;
	}

	m_verifier = std::move(pkce.verifier);
	m_status = std::move(status);
	m_done = std::move(done);
	m_alive = std::make_shared<std::atomic<bool>>(true);
	m_cancel = std::make_shared<std::atomic<bool>>(false);
	m_running = true;
	obs_log(LOG_INFO, "streamlabs login: waiting for the browser login on 127.0.0.1:%d", port);

	auto alive = m_alive;
	auto cancel_flag = m_cancel;
	UiPost post = m_post;
	m_thread = std::thread([this, listener, alive, cancel_flag, post]() {
		auto outcome = std::make_shared<Outcome>();
		const auto deadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(kStreamlabsLoginTimeoutSeconds);
		bool finished = false;
		while (!cancel_flag->load()) {
			if (std::chrono::steady_clock::now() >= deadline) {
				outcome->message = "The Streamlabs login timed out after 5 minutes. Try again.";
				finished = true;
				break;
			}
			const int ready = wait_readable(listener, kPollMs);
			if (ready < 0) {
				outcome->message = "The local login listener failed. Try again.";
				finished = true;
				break;
			}
			if (ready == 0)
				continue;
			SOCKET c = accept(listener, nullptr, nullptr);
			if (c == INVALID_SOCKET)
				continue;
			StreamlabsLoginCallback cb = read_callback(c, *cancel_flag);
			if (cb.kind == StreamlabsCallbackKind::Code) {
				send_page(c, kPageOk);
				outcome->has_code = true;
				outcome->code = std::move(cb.code);
				finished = true;
			} else if (cb.kind == StreamlabsCallbackKind::Failed) {
				send_page(c, kPageFailed);
				outcome->message = cb.error_message;
				finished = true;
			} else {
				send_not_found(c); // favicon and anything else: keep waiting
			}
			wipe(cb.code);
			shutdown(c, SD_SEND);
			closesocket(c);
			if (finished)
				break;
		}
		closesocket(listener);
		WSACleanup();
		if (!finished || !alive->load())
			return; // cancelled: nobody is told, the outcome is wiped
		post([this, alive, outcome]() {
			if (alive->load())
				on_listener_done(outcome);
		});
	});
	return true;
}

void StreamlabsLoginFlow::on_listener_done(const std::shared_ptr<Outcome> &outcome)
{
	join_thread(); // it posted this as its last step
	if (!outcome->has_code) {
		obs_log(LOG_WARNING, "streamlabs login: %s", outcome->message.c_str());
		finish(false, outcome->message);
		return;
	}
	StreamlabsClient *client = streamlabs_client();
	if (!client) {
		finish(false, "Streamlabs is not available.");
		return;
	}
	if (m_status)
		m_status("Finishing the Streamlabs login…");
	auto alive = m_alive;
	client->exchange_login(m_verifier, outcome->code, [this, alive](const StreamlabsTokenImport &r) {
		if (!alive->load())
			return; // cancelled or destroyed meanwhile; the client wipes its token copy
		if (!r.ok) {
			obs_log(LOG_WARNING, "streamlabs login: %s", r.error_message.c_str());
			finish(false, r.error_message);
			return;
		}
		std::string error;
		std::string token = r.token;
		const bool saved = streamlabs_vault::save(token, error);
		streamlabs_vault::wipe(token);
		if (!saved) {
			finish(false, error);
			return;
		}
		obs_log(LOG_INFO, "streamlabs login: connected");
		finish(true, "Logged in to Streamlabs.");
	});
	wipe(m_verifier);
	wipe(outcome->code);
}

void StreamlabsLoginFlow::finish(bool ok, const std::string &message)
{
	DoneFn done = std::move(m_done);
	m_done = nullptr;
	m_status = nullptr;
	m_running = false;
	if (m_alive)
		m_alive->store(false); // exactly one done per start
	wipe(m_verifier);
	if (done)
		done(ok, message); // may destroy or restart this flow: nothing after this line
}

/* -----------------------------------------------------------------------
 * Streamlabs Desktop import
 * ----------------------------------------------------------------------- */
bool streamlabs_desktop_import(std::string &message)
{
	wchar_t appdata[MAX_PATH];
	const DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		message = "Streamlabs Desktop was not found on this PC.";
		return false;
	}
	const std::wstring dir = std::wstring(appdata) + L"\\slobs-client\\Local Storage\\leveldb";
	const DWORD attributes = GetFileAttributesW(dir.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		message = "Streamlabs Desktop was not found on this PC.";
		return false;
	}

	/* newest *.log only */
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW((dir + L"\\*.log").c_str(), &fd);
	if (find == INVALID_HANDLE_VALUE) {
		message = "No Streamlabs Desktop login was found. Log in to Streamlabs Desktop first.";
		return false;
	}
	std::wstring newest;
	FILETIME newest_time = {};
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		if (newest.empty() || CompareFileTime(&fd.ftLastWriteTime, &newest_time) > 0) {
			newest = fd.cFileName;
			newest_time = fd.ftLastWriteTime;
		}
	} while (FindNextFileW(find, &fd));
	FindClose(find);
	if (newest.empty()) {
		message = "No Streamlabs Desktop login was found. Log in to Streamlabs Desktop first.";
		return false;
	}

	/* read-only; Streamlabs Desktop may hold the file open while it runs */
	HANDLE file = CreateFileW((dir + L"\\" + newest).c_str(), GENERIC_READ,
				  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		message = "Could not read the Streamlabs Desktop login.";
		return false;
	}
	LARGE_INTEGER size = {};
	if (!GetFileSizeEx(file, &size) || size.QuadPart > kDesktopLogMaxBytes) {
		CloseHandle(file);
		message = "The Streamlabs Desktop storage file is too large to read.";
		return false;
	}
	std::string blob(static_cast<size_t>(size.QuadPart), '\0');
	size_t got = 0;
	while (got < blob.size()) {
		DWORD read = 0;
		const DWORD chunk = static_cast<DWORD>(std::min<size_t>(blob.size() - got, 1u << 20));
		if (!ReadFile(file, &blob[got], chunk, &read, nullptr) || read == 0)
			break;
		got += read;
	}
	CloseHandle(file);
	blob.resize(got);

	StreamlabsTokenImport found = streamlabs_desktop_scan_token(blob);
	wipe(blob);
	if (!found.ok) {
		message = found.error_message;
		return false;
	}
	std::string error;
	const bool saved = streamlabs_vault::save(found.token, error);
	streamlabs_vault::wipe(found.token);
	if (!saved) {
		message = error;
		return false;
	}
	obs_log(LOG_INFO, "streamlabs login: token taken from Streamlabs Desktop");
	message = "Using the Streamlabs Desktop login.";
	return true;
}

} // namespace smulti
