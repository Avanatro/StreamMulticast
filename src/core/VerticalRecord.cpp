/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalRecord.hpp"

#include <obs-data.h>

#include <cctype>
#include <cstdio>

namespace smulti {

namespace {

bool is_sep(char c)
{
	return c == '/' || c == '\\';
}

std::string trim_trailing_separators(std::string path)
{
	while (path.size() > 1 && is_sep(path.back()))
		path.pop_back();
	return path;
}

/** Strips the Win32 long-path prefix "\\?\". */
std::string without_long_prefix(const std::string &path)
{
	return path.rfind("\\\\?\\", 0) == 0 ? path.substr(4) : path;
}

/** Drive-letter path ("E:/..." or "\\?\E:\...") or UNC ("\\server\share"). */
bool is_absolute(const std::string &path)
{
	const std::string p = without_long_prefix(path);
	if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':')
		return p.size() == 2 || is_sep(p[2]);
	return p.size() > 2 && is_sep(p[0]) && is_sep(p[1]) && !is_sep(p[2]);
}

std::string two_digits(int v)
{
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%02d", v);
	return buf;
}

} // namespace

void vertical_record_settings_save(obs_data_t *data, const VerticalRecordSettings &settings)
{
	if (!data)
		return;
	obs_data_set_bool(data, kVerticalRecordWithMainKey, settings.record_with_main);
	obs_data_set_string(data, kVerticalRecordFolderKey, settings.folder.c_str());
}

VerticalRecordSettings vertical_record_settings_load(obs_data_t *data)
{
	VerticalRecordSettings out;
	if (!data)
		return out;
	if (obs_data_has_user_value(data, kVerticalRecordWithMainKey))
		out.record_with_main = obs_data_get_bool(data, kVerticalRecordWithMainKey);
	const char *folder = obs_data_get_string(data, kVerticalRecordFolderKey);
	out.folder = folder ? folder : "";
	return out;
}

bool path_is_on_drive_c(const std::string &path)
{
	const std::string p = without_long_prefix(path);
	return p.size() >= 2 && (p[0] == 'C' || p[0] == 'c') && p[1] == ':';
}

VerticalRecordFolder resolve_vertical_record_folder(const std::string &main_record_path, const std::string &configured)
{
	VerticalRecordFolder out;
	std::string folder;
	if (!configured.empty()) {
		folder = trim_trailing_separators(configured);
	} else {
		if (main_record_path.empty()) {
			out.reason = "The main recording path of the active profile is not known.";
			return out;
		}
		folder = trim_trailing_separators(main_record_path) + "/Vertical";
	}

	if (!is_absolute(folder)) {
		out.reason = "The recording folder \"" + folder + "\" is not an absolute path.";
		return out;
	}
	if (path_is_on_drive_c(folder)) {
		out.reason = "The recording folder \"" + folder + "\" is on drive C:; recordings never go to C:.";
		return out;
	}
	out.ok = true;
	out.folder = folder;
	return out;
}

std::string format_record_file_stem(const std::string &format, const std::tm &time)
{
	struct Code {
		const char *code;
		std::string value;
	};
	const Code codes[] = {
		{"%CCYY", std::to_string(time.tm_year + 1900)},
		{"%YY", two_digits((time.tm_year + 1900) % 100)},
		{"%MM", two_digits(time.tm_mon + 1)},
		{"%DD", two_digits(time.tm_mday)},
		{"%hh", two_digits(time.tm_hour)},
		{"%mm", two_digits(time.tm_min)},
		{"%ss", two_digits(time.tm_sec)},
		{"%%", "%"},
	};

	std::string out;
	size_t i = 0;
	while (i < format.size()) {
		bool matched = false;
		if (format[i] == '%') {
			for (const Code &c : codes) {
				const std::string code = c.code;
				if (format.compare(i, code.size(), code) == 0) {
					out += c.value;
					i += code.size();
					matched = true;
					break;
				}
			}
		}
		if (!matched)
			out += format[i++];
	}
	return out;
}

std::string vertical_record_file_path(const std::string &folder, const std::string &stem,
				      const std::function<bool(const std::string &)> &exists)
{
	const std::string base = trim_trailing_separators(folder) + "/" + stem;
	std::string path = base + ".mkv";
	for (int n = 2; exists && exists(path) && n < 1000; ++n)
		path = base + " (" + std::to_string(n) + ").mkv";
	return path;
}

std::string remux_target_path(const std::string &recorded_path)
{
	const size_t dot = recorded_path.find_last_of('.');
	const size_t sep = recorded_path.find_last_of("/\\");
	if (dot == std::string::npos || (sep != std::string::npos && dot < sep))
		return recorded_path + ".mp4";
	return recorded_path.substr(0, dot) + ".mp4";
}

RecordEncoderChoice choose_vertical_record_encoder(const std::set<std::string> &available)
{
	RecordEncoderChoice c;
	for (const char *id : {"obs_nvenc_hevc_tex", "jim_hevc_nvenc"}) {
		if (available.count(id)) {
			c.type_id = id;
			c.rate_control = "CQP";
			c.quality_key = "cqp";
			c.quality = 20;
			return c;
		}
	}
	c.type_id = "obs_x264";
	c.rate_control = "CRF";
	c.quality_key = "crf";
	c.quality = 20;
	return c;
}

} // namespace smulti
