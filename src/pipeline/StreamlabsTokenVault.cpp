/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "StreamlabsTokenVault.hpp"
#include "../core/StreamlabsTikTok.hpp"

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
#include <wincrypt.h>

#include <vector>

namespace smulti {
namespace streamlabs_vault {

namespace {

constexpr const char *kFileName = "streamlabs_tiktok.bin";
constexpr DWORD kMaxFileBytes = 64 * 1024;

/* Constant DPAPI entropy: binds the blob to this plugin on top of the user scope. */
const BYTE kEntropy[] = "StreamMulticast.StreamlabsTikTok.v1";

std::wstring to_wide(const std::string &utf8)
{
	wchar_t *w = nullptr;
	os_utf8_to_wcs_ptr(utf8.c_str(), utf8.size(), &w);
	std::wstring out = w ? w : L"";
	bfree(w);
	return out;
}

/* Full path of the vault file, creating the plugin config dir. "" when unavailable. */
std::string vault_path()
{
	char *dir = obs_module_config_path("");
	if (!dir)
		return {};
	os_mkdirs(dir);
	bfree(dir);
	char *path = obs_module_config_path(kFileName);
	if (!path)
		return {};
	std::string out = path;
	bfree(path);
	return out;
}

DATA_BLOB entropy_blob()
{
	DATA_BLOB e;
	e.pbData = const_cast<BYTE *>(kEntropy);
	e.cbData = static_cast<DWORD>(sizeof(kEntropy) - 1);
	return e;
}

void free_blob(DATA_BLOB &blob, bool wipe_first)
{
	if (!blob.pbData)
		return;
	if (wipe_first)
		SecureZeroMemory(blob.pbData, blob.cbData);
	LocalFree(blob.pbData);
	blob.pbData = nullptr;
	blob.cbData = 0;
}

} // namespace

void wipe(std::string &secret)
{
	if (!secret.empty())
		SecureZeroMemory(&secret[0], secret.size());
	secret.clear();
}

bool save(const std::string &token, std::string &error)
{
	error.clear();
	if (token.empty() || token.size() > kStreamlabsTokenMax) {
		error = "The token is empty or too long.";
		return false;
	}
	const std::string path = vault_path();
	if (path.empty()) {
		error = "The plugin config folder is not available.";
		return false;
	}

	DATA_BLOB in;
	in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(token.data()));
	in.cbData = static_cast<DWORD>(token.size());
	DATA_BLOB entropy = entropy_blob();
	DATA_BLOB out = {};
	if (!CryptProtectData(&in, L"StreamMulticast Streamlabs token", &entropy, nullptr, nullptr,
			      CRYPTPROTECT_UI_FORBIDDEN, &out)) {
		error = "Windows could not encrypt the token (DPAPI error " + std::to_string(GetLastError()) + ").";
		return false;
	}

	/* Atomic write: temp file next to the target, flushed, then renamed over it. */
	const std::wstring target = to_wide(path);
	const std::wstring temp = target + L".tmp";
	bool ok = false;
	HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f != INVALID_HANDLE_VALUE) {
		DWORD written = 0;
		ok = WriteFile(f, out.pbData, out.cbData, &written, nullptr) && written == out.cbData &&
		     FlushFileBuffers(f);
		CloseHandle(f);
		if (ok)
			ok = MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
			     0;
		if (!ok)
			DeleteFileW(temp.c_str());
	}
	free_blob(out, false);
	if (!ok)
		error = "Could not write the token file in the plugin config folder.";
	return ok;
}

bool load(std::string &token_out)
{
	wipe(token_out);
	const std::string path = vault_path();
	if (path.empty())
		return false;

	HANDLE f = CreateFileW(to_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return false;
	std::vector<BYTE> blob;
	LARGE_INTEGER size = {};
	bool read_ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart <= kMaxFileBytes;
	if (read_ok) {
		blob.resize(static_cast<size_t>(size.QuadPart));
		DWORD got = 0;
		read_ok = ReadFile(f, blob.data(), static_cast<DWORD>(blob.size()), &got, nullptr) &&
			  got == blob.size();
	}
	CloseHandle(f);
	if (!read_ok)
		return false;

	DATA_BLOB in;
	in.pbData = blob.data();
	in.cbData = static_cast<DWORD>(blob.size());
	DATA_BLOB entropy = entropy_blob();
	DATA_BLOB out = {};
	if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return false;

	bool ok = out.cbData > 0 && out.cbData <= kStreamlabsTokenMax;
	if (ok)
		token_out.assign(reinterpret_cast<const char *>(out.pbData), out.cbData);
	free_blob(out, true);
	return ok;
}

bool remove()
{
	const std::string path = vault_path();
	if (path.empty())
		return false;
	const std::wstring target = to_wide(path);
	if (DeleteFileW(target.c_str()))
		return true;
	return GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool exists()
{
	const std::string path = vault_path();
	return !path.empty() && os_file_exists(path.c_str());
}

} // namespace streamlabs_vault
} // namespace smulti
