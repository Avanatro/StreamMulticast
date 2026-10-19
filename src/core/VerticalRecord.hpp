/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <ctime>
#include <functional>
#include <set>
#include <string>

// libobs forward declaration (obs-data only, OBS-free harness safe)
struct obs_data;
typedef struct obs_data obs_data_t;

namespace smulti {

/**
 * Pure parts of the vertical recording (recording of the owned canvas to a file): settings, target
 * folder with the drive C: refusal, file name, remux target and encoder choice. The OBS adapter is
 * pipeline/VerticalRecorder.
 */

/** Keys of the plugin's vertical-record settings file. */
inline constexpr const char *kVerticalRecordWithMainKey = "record_with_main";
inline constexpr const char *kVerticalRecordFolderKey = "folder";

struct VerticalRecordSettings {
	bool record_with_main = true; ///< start/stop together with the OBS main recording
	std::string folder;           ///< empty: <main recording path>/Vertical
};

void vertical_record_settings_save(obs_data_t *data, const VerticalRecordSettings &settings);

/** Missing keys yield the defaults (record with main, default folder). */
VerticalRecordSettings vertical_record_settings_load(obs_data_t *data);

/** True for a path on drive C: ("C:..." or "\\?\C:...", any case). */
bool path_is_on_drive_c(const std::string &path);

struct VerticalRecordFolder {
	bool ok = false;
	std::string folder; ///< without a trailing separator
	std::string reason; ///< why it was refused (ok == false)
};

/**
 * The folder the vertical recording is written to: `configured` when not empty, else
 * `main_record_path` + "/Vertical". Refused when it is empty, not absolute (drive letter or UNC) or on
 * drive C: (recordings never go to the system drive).
 */
VerticalRecordFolder resolve_vertical_record_folder(const std::string &main_record_path, const std::string &configured);

/** OBS-style file name format of the vertical recording, without the extension. */
inline constexpr const char *kVerticalRecordFileStemFormat = "%CCYY-%MM-%DD %hh-%mm-%ss Vertical";

/** Expands %CCYY %YY %MM %DD %hh %mm %ss and %% with `time`; other text, unknown codes included, is kept. */
std::string format_record_file_stem(const std::string &format, const std::tm &time);

/** "<folder>/<stem>.mkv", or "<folder>/<stem> (N).mkv" with the first N >= 2 for which `exists` is false. */
std::string vertical_record_file_path(const std::string &folder, const std::string &stem,
				      const std::function<bool(const std::string &)> &exists);

/** The MP4 path an automatic remux writes: the extension (if any) replaced by ".mp4". */
std::string remux_target_path(const std::string &recorded_path);

/** Video encoder and quality-based rate control for the vertical recording. */
struct RecordEncoderChoice {
	std::string type_id;
	std::string rate_control; ///< "CQP" (NVENC) or "CRF" (x264)
	std::string quality_key;  ///< settings key of the quality value: "cqp" or "crf"
	int quality = 20;
};

/**
 * NVENC HEVC CQP 20 when available (obs_nvenc_hevc_tex, then the compat id jim_hevc_nvenc), else
 * x264 CRF 20. `available` is the set of registered encoder type ids.
 */
RecordEncoderChoice choose_vertical_record_encoder(const std::set<std::string> &available);

} // namespace smulti
