/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <ctime>
#include <functional>
#include <optional>
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

/** Keys of the vertical replay buffer, inside the "replay" object of the same settings file. */
inline constexpr const char *kVerticalReplayKey = "replay";
inline constexpr const char *kVerticalReplayWithMainKey = "enabled_with_main_replay";
inline constexpr const char *kVerticalReplaySecondsKey = "seconds";
inline constexpr const char *kVerticalReplayMaxSizeKey = "max_size_mb";
inline constexpr const char *kVerticalReplayFolderKey = "folder";

inline constexpr int kVerticalReplayMinSeconds = 30;
inline constexpr int kVerticalReplayMaxSeconds = 60;
inline constexpr int kVerticalReplayMinSizeMb = 64;
inline constexpr int kVerticalReplayMaxSizeMb = 2048;

/** Vertical replay buffer (pipeline/VerticalReplayBuffer): clips of the owned canvas on a hotkey. */
struct VerticalReplaySettings {
	bool enabled_with_main_replay = true; ///< start/stop together with the OBS main replay buffer
	int seconds = 30;                     ///< buffer length, clamped to 30..60
	int max_size_mb = 512;                ///< memory cap of the buffer, clamped to 64..2048
	std::string folder = "E:\\Videos\\Clips\\vertical"; ///< resolved by resolve_vertical_record_folder (never C:)
};

/** `settings` with seconds and max_size_mb clamped into their ranges. */
VerticalReplaySettings clamp_vertical_replay_settings(VerticalReplaySettings settings);

struct VerticalRecordSettings {
	bool record_with_main = true; ///< start/stop together with the OBS main recording
	std::string folder;           ///< empty: <main recording path>/Vertical
	VerticalReplaySettings replay; ///< saved as the "replay" object
};

void vertical_record_settings_save(obs_data_t *data, const VerticalRecordSettings &settings);

/** Missing keys yield the defaults (record with main, default folder; replay defaults, clamped). */
VerticalRecordSettings vertical_record_settings_load(obs_data_t *data);

/**
 * True for a path on drive C:, any case: "C:...", the namespace forms "\\?\C:..." and "\\.\C:...", and the
 * administrative share "\\<host>\C$\..." (also as "\\?\UNC\<host>\C$\...").
 */
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

/**
 * "<folder>/<stem><extension>", or "<folder>/<stem> (N)<extension>" with the first N >= 2 for which `exists`
 * is false. `extension` includes the dot.
 */
std::string vertical_record_file_path(const std::string &folder, const std::string &stem,
				      const std::function<bool(const std::string &)> &exists,
				      const std::string &extension = ".mkv");

/** Candidate `n` of vertical_record_file_path: n <= 1 "<folder>/<stem><extension>", else "... (n)<extension>". */
std::string vertical_record_numbered_path(const std::string &folder, const std::string &stem, int n,
					  const std::string &extension);

/** Local time of `t` (the date and time parts of the file names). */
std::tm local_time(std::time_t t);

/**
 * File name stem of a vertical clip: "<CCYY-MM-DD hh-mm-ss>-<scene>". The scene name is made safe for
 * Windows file names: \ / : * ? " < > | and control characters become '_', it is cut to 80 bytes (never
 * inside a UTF-8 sequence), trailing dots and spaces are trimmed, and an empty result becomes "vertical".
 */
std::string clip_file_stem(const std::tm &date, const std::string &scene_name);

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

/**
 * The encoder to retry with after `failed_type_id` failed at encoder create or output start (NVENC
 * registered but unusable: session limit, init error): choose_vertical_record_encoder without any NVENC
 * HEVC id, i.e. x264 CRF 20. nullopt when the failed encoder was not NVENC (nothing left to try).
 */
std::optional<RecordEncoderChoice> vertical_record_fallback_encoder(const std::string &failed_type_id,
								    std::set<std::string> available);

/**
 * The encoder to restart a running recording with after it stopped with `stop_code`: only an encode error
 * (OBS_OUTPUT_ENCODE_ERROR) of a session that ran with an NVENC HEVC id and was not restarted before gives
 * vertical_record_fallback_encoder (x264 CRF 20), restarted once into a new file; nullopt otherwise (the
 * recording ends as before).
 */
std::optional<RecordEncoderChoice> vertical_record_runtime_fallback(long long stop_code, const std::string &session_type_id,
								    bool already_restarted,
								    const std::set<std::string> &available);

} // namespace smulti
