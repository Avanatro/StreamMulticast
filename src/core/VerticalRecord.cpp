/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalRecord.hpp"
#include "EncoderRecovery.hpp"

#include <obs-data.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace smulti {

namespace {

/** NVENC HEVC encoder ids in preference order (the new API id, then the compat id). */
constexpr const char *kNvencHevcIds[] = {"obs_nvenc_hevc_tex", "jim_hevc_nvenc"};

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

/**
 * Strips the Win32 namespace prefixes "\\?\" and "\\.\" (either separator); "\\?\UNC\host\share" and
 * "\\.\UNC\host\share" become "\\host\share".
 */
std::string without_namespace_prefix(const std::string &path)
{
	const std::string &p = path;
	if (p.size() < 4 || !is_sep(p[0]) || !is_sep(p[1]) || (p[2] != '?' && p[2] != '.') || !is_sep(p[3]))
		return path;
	const std::string rest = p.substr(4);
	if (rest.size() >= 4 && std::toupper(static_cast<unsigned char>(rest[0])) == 'U' &&
	    std::toupper(static_cast<unsigned char>(rest[1])) == 'N' &&
	    std::toupper(static_cast<unsigned char>(rest[2])) == 'C' && is_sep(rest[3]))
		return "\\\\" + rest.substr(4);
	return rest;
}

/** Share name of a UNC path "\\host\share\..." ("" when `p` is not UNC). */
std::string unc_share(const std::string &p)
{
	if (p.size() < 3 || !is_sep(p[0]) || !is_sep(p[1]) || is_sep(p[2]))
		return "";
	size_t host_end = 2;
	while (host_end < p.size() && !is_sep(p[host_end]))
		++host_end;
	if (host_end >= p.size())
		return "";
	size_t share_end = host_end + 1;
	while (share_end < p.size() && !is_sep(p[share_end]))
		++share_end;
	return p.substr(host_end + 1, share_end - host_end - 1);
}

/** Drive-letter path ("E:/...", "\\?\E:\...", "\\.\E:\...") or UNC ("\\server\share"). */
bool is_absolute(const std::string &path)
{
	const std::string p = without_namespace_prefix(path);
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

	const VerticalReplaySettings r = clamp_vertical_replay_settings(settings.replay);
	obs_data_t *replay = obs_data_create();
	obs_data_set_bool(replay, kVerticalReplayWithMainKey, r.enabled_with_main_replay);
	obs_data_set_int(replay, kVerticalReplaySecondsKey, r.seconds);
	obs_data_set_int(replay, kVerticalReplayMaxSizeKey, r.max_size_mb);
	obs_data_set_string(replay, kVerticalReplayFolderKey, r.folder.c_str());
	obs_data_set_obj(data, kVerticalReplayKey, replay);
	obs_data_release(replay);
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

	/* A file written before the replay buffer existed has no "replay" object: defaults. */
	obs_data_t *replay = obs_data_get_obj(data, kVerticalReplayKey);
	if (replay) {
		VerticalReplaySettings &r = out.replay;
		if (obs_data_has_user_value(replay, kVerticalReplayWithMainKey))
			r.enabled_with_main_replay = obs_data_get_bool(replay, kVerticalReplayWithMainKey);
		if (obs_data_has_user_value(replay, kVerticalReplaySecondsKey))
			r.seconds = static_cast<int>(obs_data_get_int(replay, kVerticalReplaySecondsKey));
		if (obs_data_has_user_value(replay, kVerticalReplayMaxSizeKey))
			r.max_size_mb = static_cast<int>(obs_data_get_int(replay, kVerticalReplayMaxSizeKey));
		if (obs_data_has_user_value(replay, kVerticalReplayFolderKey)) {
			const char *f = obs_data_get_string(replay, kVerticalReplayFolderKey);
			r.folder = f ? f : "";
		}
		obs_data_release(replay);
	}
	out.replay = clamp_vertical_replay_settings(out.replay);
	return out;
}

VerticalReplaySettings clamp_vertical_replay_settings(VerticalReplaySettings settings)
{
	settings.seconds = std::clamp(settings.seconds, kVerticalReplayMinSeconds, kVerticalReplayMaxSeconds);
	settings.max_size_mb = std::clamp(settings.max_size_mb, kVerticalReplayMinSizeMb, kVerticalReplayMaxSizeMb);
	return settings;
}

bool path_is_on_drive_c(const std::string &path)
{
	const std::string p = without_namespace_prefix(path);
	if (p.size() >= 2 && (p[0] == 'C' || p[0] == 'c') && p[1] == ':')
		return true;
	/* The administrative share of drive C ("\\host\C$\...", any host, localhost included). */
	const std::string share = unc_share(p);
	return share.size() == 2 && (share[0] == 'C' || share[0] == 'c') && share[1] == '$';
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
				      const std::function<bool(const std::string &)> &exists, const std::string &extension)
{
	std::string path = vertical_record_numbered_path(folder, stem, 1, extension);
	for (int n = 2; exists && exists(path) && n < 1000; ++n)
		path = vertical_record_numbered_path(folder, stem, n, extension);
	return path;
}

std::string vertical_record_numbered_path(const std::string &folder, const std::string &stem, int n,
					  const std::string &extension)
{
	const std::string base = trim_trailing_separators(folder) + "/" + stem;
	return n <= 1 ? base + extension : base + " (" + std::to_string(n) + ")" + extension;
}

std::tm local_time(std::time_t t)
{
	std::tm out = {};
#ifdef _WIN32
	localtime_s(&out, &t);
#else
	localtime_r(&t, &out);
#endif
	return out;
}

std::string clip_file_stem(const std::tm &date, const std::string &scene_name)
{
	constexpr size_t kMaxSceneBytes = 80;
	std::string scene;
	scene.reserve(scene_name.size());
	for (char ch : scene_name) {
		const unsigned char c = static_cast<unsigned char>(ch);
		const bool forbidden = c < 0x20 || c == 0x7F || std::string("\\/:*?\"<>|").find(ch) != std::string::npos;
		scene += forbidden ? '_' : ch;
	}
	if (scene.size() > kMaxSceneBytes) {
		size_t cut = kMaxSceneBytes;
		/* back off over UTF-8 continuation bytes (10xxxxxx) so a multi-byte character is never split */
		while (cut > 0 && (static_cast<unsigned char>(scene[cut]) & 0xC0) == 0x80)
			--cut;
		scene.resize(cut);
	}
	while (!scene.empty() && (scene.back() == '.' || scene.back() == ' '))
		scene.pop_back();
	if (scene.empty())
		scene = "vertical";
	return format_record_file_stem("%CCYY-%MM-%DD %hh-%mm-%ss", date) + "-" + scene;
}

std::string remux_target_path(const std::string &recorded_path)
{
	const size_t dot = recorded_path.find_last_of('.');
	const size_t sep = recorded_path.find_last_of("/\\");
	if (dot == std::string::npos || (sep != std::string::npos && dot < sep))
		return recorded_path + ".mp4";
	return recorded_path.substr(0, dot) + ".mp4";
}

std::optional<RecordEncoderChoice> vertical_record_fallback_encoder(const std::string &failed_type_id,
								    std::set<std::string> available)
{
	bool failed_was_nvenc = false;
	for (const char *id : kNvencHevcIds) {
		failed_was_nvenc = failed_was_nvenc || failed_type_id == id;
		available.erase(id);
	}
	if (!failed_was_nvenc)
		return std::nullopt;
	return choose_vertical_record_encoder(available);
}

std::optional<RecordEncoderChoice> vertical_record_runtime_fallback(long long stop_code, const std::string &session_type_id,
								    bool already_restarted,
								    const std::set<std::string> &available)
{
	if (stop_code != kStopCodeEncodeError || already_restarted)
		return std::nullopt;
	return vertical_record_fallback_encoder(session_type_id, available);
}

RecordEncoderChoice choose_vertical_record_encoder(const std::set<std::string> &available)
{
	RecordEncoderChoice c;
	for (const char *id : kNvencHevcIds) {
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
