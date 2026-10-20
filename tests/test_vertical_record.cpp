// Vertical recording: pure folder / file name resolution, C: refusal, encoder choice, settings. obs_data only.
#include "TestRunner.hpp"
#include "core/VerticalRecord.hpp"

#include <obs-data.h>

#include <ctime>
#include <optional>
#include <set>
#include <string>

using smulti::RecordEncoderChoice;
using smulti::VerticalRecordFolder;
using smulti::VerticalRecordSettings;

namespace {

std::tm fixed_time()
{
	std::tm t = {};
	t.tm_year = 2026 - 1900;
	t.tm_mon = 9; // October
	t.tm_mday = 19;
	t.tm_hour = 7;
	t.tm_min = 5;
	t.tm_sec = 9;
	return t;
}

} // namespace

SMULTI_TEST(vertical_record_default_folder_is_main_path_vertical)
{
	VerticalRecordFolder f = smulti::resolve_vertical_record_folder("E:/Recordings", "");
	EXPECT_TRUE(f.ok);
	EXPECT_EQ(f.folder, "E:/Recordings/Vertical");
	/* trailing separators are not doubled */
	EXPECT_EQ(smulti::resolve_vertical_record_folder("E:\\Recordings\\", "").folder, "E:\\Recordings/Vertical");
	/* a configured folder wins over the default */
	VerticalRecordFolder c = smulti::resolve_vertical_record_folder("E:/Recordings", "F:/Shorts/");
	EXPECT_TRUE(c.ok);
	EXPECT_EQ(c.folder, "F:/Shorts");
}

SMULTI_TEST(vertical_record_refuses_drive_c)
{
	EXPECT_TRUE(smulti::path_is_on_drive_c("C:/Users/Avana/Videos"));
	EXPECT_TRUE(smulti::path_is_on_drive_c("c:\\Videos"));
	EXPECT_TRUE(smulti::path_is_on_drive_c("\\\\?\\C:\\Videos"));
	EXPECT_TRUE(!smulti::path_is_on_drive_c("E:/Recordings"));
	EXPECT_TRUE(!smulti::path_is_on_drive_c("\\\\nas\\share\\C:"));
	/* device namespace and administrative C$ shares are drive C: too */
	EXPECT_TRUE(smulti::path_is_on_drive_c("\\\\.\\C:\\Rec"));
	EXPECT_TRUE(smulti::path_is_on_drive_c("\\\\?\\UNC\\host\\C$\\Rec"));
	EXPECT_TRUE(smulti::path_is_on_drive_c("\\\\localhost\\C$\\Rec"));
	EXPECT_TRUE(smulti::path_is_on_drive_c("//localhost/c$"));
	EXPECT_TRUE(!smulti::path_is_on_drive_c("\\\\nas\\D$\\Rec"));
	EXPECT_TRUE(!smulti::path_is_on_drive_c("\\\\nas\\C$x\\Rec"));
	EXPECT_TRUE(!smulti::resolve_vertical_record_folder("", "\\\\localhost\\C$\\Rec").ok);
	EXPECT_TRUE(!smulti::resolve_vertical_record_folder("", "\\\\.\\C:\\Rec").ok);
	EXPECT_TRUE(smulti::resolve_vertical_record_folder("", "\\\\.\\E:\\Rec").ok);

	VerticalRecordFolder d = smulti::resolve_vertical_record_folder("C:/Users/Avana/Videos", "");
	EXPECT_TRUE(!d.ok);
	EXPECT_TRUE(d.reason.find("C:") != std::string::npos);
	EXPECT_TRUE(!smulti::resolve_vertical_record_folder("E:/Recordings", "C:/Temp").ok);
}

SMULTI_TEST(vertical_record_refuses_unknown_or_relative_folder)
{
	EXPECT_TRUE(!smulti::resolve_vertical_record_folder("", "").ok);
	EXPECT_TRUE(!smulti::resolve_vertical_record_folder("Recordings", "").ok);
	EXPECT_TRUE(!smulti::resolve_vertical_record_folder("E:/Recordings", "relative/dir").ok);
	EXPECT_TRUE(smulti::resolve_vertical_record_folder("", "\\\\nas\\share\\vertical").ok); // UNC is absolute
}

SMULTI_TEST(vertical_record_file_name_format)
{
	EXPECT_EQ(smulti::format_record_file_stem(smulti::kVerticalRecordFileStemFormat, fixed_time()),
		  "2026-10-19 07-05-09 Vertical");
	EXPECT_EQ(smulti::format_record_file_stem("%YY %% %x", fixed_time()), "26 % %x");
	EXPECT_EQ(smulti::vertical_record_file_path("E:/Recordings/Vertical", "2026-10-19 07-05-09 Vertical",
						    [](const std::string &) { return false; }),
		  "E:/Recordings/Vertical/2026-10-19 07-05-09 Vertical.mkv");
}

SMULTI_TEST(vertical_record_file_path_avoids_existing_files)
{
	std::set<std::string> existing = {"E:/V/a.mkv", "E:/V/a (2).mkv"};
	auto exists = [&existing](const std::string &p) { return existing.count(p) != 0; };
	EXPECT_EQ(smulti::vertical_record_file_path("E:/V", "a", exists), "E:/V/a (3).mkv");
}

SMULTI_TEST(vertical_record_remux_target_is_mp4)
{
	EXPECT_EQ(smulti::remux_target_path("E:/V/a.mkv"), "E:/V/a.mp4");
	EXPECT_EQ(smulti::remux_target_path("E:/V/a.MKV"), "E:/V/a.mp4");
	EXPECT_EQ(smulti::remux_target_path("E:/V/a"), "E:/V/a.mp4");
}

SMULTI_TEST(vertical_record_encoder_prefers_nvenc_hevc)
{
	RecordEncoderChoice a =
		smulti::choose_vertical_record_encoder({"obs_x264", "jim_hevc_nvenc", "obs_nvenc_hevc_tex"});
	EXPECT_EQ(a.type_id, "obs_nvenc_hevc_tex");
	EXPECT_EQ(a.rate_control, "CQP");
	EXPECT_EQ(a.quality_key, "cqp");
	EXPECT_EQ(a.quality, 20);

	RecordEncoderChoice b = smulti::choose_vertical_record_encoder({"obs_x264", "jim_hevc_nvenc"});
	EXPECT_EQ(b.type_id, "jim_hevc_nvenc");
	EXPECT_EQ(b.rate_control, "CQP");
}

SMULTI_TEST(vertical_record_encoder_falls_back_to_x264_crf)
{
	RecordEncoderChoice c = smulti::choose_vertical_record_encoder({"obs_x264", "jim_nvenc"}); // H.264 NVENC only
	EXPECT_EQ(c.type_id, "obs_x264");
	EXPECT_EQ(c.rate_control, "CRF");
	EXPECT_EQ(c.quality_key, "crf");
	EXPECT_EQ(c.quality, 20);
	EXPECT_EQ(smulti::choose_vertical_record_encoder({}).type_id, "obs_x264");
}

SMULTI_TEST(vertical_record_encoder_fallback_after_nvenc_failure)
{
	const std::set<std::string> all = {"obs_x264", "jim_hevc_nvenc", "obs_nvenc_hevc_tex"};
	/* NVENC registered but failed at create or start: retry once with x264 CRF 20 */
	std::optional<RecordEncoderChoice> f = smulti::vertical_record_fallback_encoder("obs_nvenc_hevc_tex", all);
	EXPECT_TRUE(f.has_value());
	EXPECT_EQ(f ? f->type_id : std::string(), "obs_x264");
	EXPECT_EQ(f ? f->rate_control : std::string(), "CRF");
	EXPECT_EQ(f ? f->quality : 0, 20);
	/* the compat id is NVENC too: never retried with the other NVENC id */
	std::optional<RecordEncoderChoice> g = smulti::vertical_record_fallback_encoder("jim_hevc_nvenc", all);
	EXPECT_EQ(g ? g->type_id : std::string(), "obs_x264");
	/* x264 itself failed: nothing left to try */
	EXPECT_TRUE(!smulti::vertical_record_fallback_encoder("obs_x264", all).has_value());
}

SMULTI_TEST(vertical_record_settings_roundtrip_and_defaults)
{
	obs_data_t *empty = obs_data_create();
	VerticalRecordSettings d = smulti::vertical_record_settings_load(empty);
	obs_data_release(empty);
	EXPECT_EQ(d.record_with_main, true);
	EXPECT_EQ(d.folder, "");

	VerticalRecordSettings s;
	s.record_with_main = false;
	s.folder = "F:/Shorts";
	obs_data_t *data = obs_data_create();
	smulti::vertical_record_settings_save(data, s);
	VerticalRecordSettings back = smulti::vertical_record_settings_load(data);
	obs_data_release(data);
	EXPECT_EQ(back.record_with_main, false);
	EXPECT_EQ(back.folder, "F:/Shorts");
}

SMULTI_TEST(vertical_record_runtime_fallback_choice)
{
	const std::set<std::string> all = {"obs_x264", "jim_hevc_nvenc", "obs_nvenc_hevc_tex"};
	const long long encode_error = -8; // OBS_OUTPUT_ENCODE_ERROR
	/* NVENC session hit an encode error after a successful start: restart once with x264 CRF 20 */
	std::optional<RecordEncoderChoice> r =
		smulti::vertical_record_runtime_fallback(encode_error, "obs_nvenc_hevc_tex", false, all);
	EXPECT_TRUE(r.has_value());
	EXPECT_EQ(r ? r->type_id : std::string(), "obs_x264");
	EXPECT_EQ(r ? r->rate_control : std::string(), "CRF");
	EXPECT_EQ(r ? r->quality : 0, 20);
	EXPECT_EQ(smulti::vertical_record_runtime_fallback(encode_error, "jim_hevc_nvenc", false, all)
			  .value_or(RecordEncoderChoice{}).type_id,
		  "obs_x264");
	/* a second encode error (already restarted) ends the recording */
	EXPECT_TRUE(!smulti::vertical_record_runtime_fallback(encode_error, "obs_nvenc_hevc_tex", true, all).has_value());
	/* an x264 session has nothing left to try */
	EXPECT_TRUE(!smulti::vertical_record_runtime_fallback(encode_error, "obs_x264", false, all).has_value());
	/* other stop codes never restart: success, disconnected, generic error, no space */
	for (long long code : {0LL, -5LL, -4LL, -7LL})
		EXPECT_TRUE(!smulti::vertical_record_runtime_fallback(code, "obs_nvenc_hevc_tex", false, all).has_value());
}

/* ---- vertical replay buffer (TASK-36) ---- */

SMULTI_TEST(vertical_replay_settings_roundtrip_and_defaults)
{
	/* defaults, also for a file written before the "replay" object existed */
	obs_data_t *old_file = obs_data_create();
	obs_data_set_bool(old_file, "record_with_main", false);
	obs_data_set_string(old_file, "folder", "F:/Shorts");
	const VerticalRecordSettings old = smulti::vertical_record_settings_load(old_file);
	obs_data_release(old_file);
	EXPECT_EQ(old.record_with_main, false);
	EXPECT_EQ(old.replay.enabled_with_main_replay, true);
	EXPECT_EQ(old.replay.seconds, 30);
	EXPECT_EQ(old.replay.max_size_mb, 512);
	EXPECT_EQ(old.replay.folder, "E:\\Videos\\Clips\\vertical");

	VerticalRecordSettings s;
	s.replay.enabled_with_main_replay = false;
	s.replay.seconds = 45;
	s.replay.max_size_mb = 1024;
	s.replay.folder = "F:/Clips";
	obs_data_t *data = obs_data_create();
	smulti::vertical_record_settings_save(data, s);
	/* one "replay" object in the same file */
	obs_data_t *replay = obs_data_get_obj(data, "replay");
	EXPECT_TRUE(replay != nullptr);
	EXPECT_EQ(obs_data_get_int(replay, "seconds"), 45);
	obs_data_release(replay);
	const VerticalRecordSettings back = smulti::vertical_record_settings_load(data);
	obs_data_release(data);
	EXPECT_EQ(back.record_with_main, true);
	EXPECT_EQ(back.replay.enabled_with_main_replay, false);
	EXPECT_EQ(back.replay.seconds, 45);
	EXPECT_EQ(back.replay.max_size_mb, 1024);
	EXPECT_EQ(back.replay.folder, "F:/Clips");
}

SMULTI_TEST(vertical_replay_seconds_clamped)
{
	smulti::VerticalReplaySettings s;
	s.seconds = 5;
	s.max_size_mb = 10;
	smulti::VerticalReplaySettings c = smulti::clamp_vertical_replay_settings(s);
	EXPECT_EQ(c.seconds, 30);
	EXPECT_EQ(c.max_size_mb, 64);
	s.seconds = 600;
	s.max_size_mb = 100000;
	c = smulti::clamp_vertical_replay_settings(s);
	EXPECT_EQ(c.seconds, 60);
	EXPECT_EQ(c.max_size_mb, 2048);
	s.seconds = 42;
	s.max_size_mb = 300;
	c = smulti::clamp_vertical_replay_settings(s);
	EXPECT_EQ(c.seconds, 42);
	EXPECT_EQ(c.max_size_mb, 300);

	/* a hand-edited file is clamped on load */
	obs_data_t *data = obs_data_create();
	obs_data_t *replay = obs_data_create();
	obs_data_set_int(replay, "seconds", 1);
	obs_data_set_int(replay, "max_size_mb", 99999);
	obs_data_set_obj(data, "replay", replay);
	obs_data_release(replay);
	const VerticalRecordSettings loaded = smulti::vertical_record_settings_load(data);
	obs_data_release(data);
	EXPECT_EQ(loaded.replay.seconds, 30);
	EXPECT_EQ(loaded.replay.max_size_mb, 2048);
}

SMULTI_TEST(vertical_replay_folder_refuses_drive_c)
{
	/* the default clip folder is accepted */
	const VerticalRecordFolder def =
		smulti::resolve_vertical_record_folder("E:/Videos/Stream", smulti::VerticalReplaySettings{}.folder);
	EXPECT_TRUE(def.ok);
	EXPECT_EQ(def.folder, "E:\\Videos\\Clips\\vertical");
	/* every C: form is refused, with the recording's message */
	for (const char *c : {"C:\\Videos\\Clips", "c:/clips", "\\\\?\\C:\\Clips", "\\\\localhost\\C$\\Clips"}) {
		const VerticalRecordFolder f = smulti::resolve_vertical_record_folder("E:/Videos/Stream", c);
		EXPECT_TRUE(!f.ok);
		EXPECT_TRUE(f.reason.find("never go to C:") != std::string::npos);
	}
	/* an empty clip folder never falls back to a C: main path either */
	EXPECT_TRUE(!smulti::resolve_vertical_record_folder("C:/Users/Avana/Videos", "").ok);
}

SMULTI_TEST(vertical_clip_stem_has_date_and_scene)
{
	EXPECT_EQ(smulti::clip_file_stem(fixed_time(), "V Gaming"), "2026-10-19 07-05-09-V Gaming");
	EXPECT_EQ(smulti::clip_file_stem(fixed_time(), ""), "2026-10-19 07-05-09-vertical");
	/* local_time gives the calendar parts the stem uses */
	const std::tm t = smulti::local_time(std::time(nullptr));
	EXPECT_TRUE(t.tm_year >= 2026 - 1900);
	EXPECT_TRUE(t.tm_mday >= 1);
}

SMULTI_TEST(vertical_clip_stem_sanitizes_scene_name)
{
	const std::tm t = fixed_time();
	const std::string prefix = "2026-10-19 07-05-09-";
	EXPECT_EQ(smulti::clip_file_stem(t, "a\\b/c:d*e?f\"g<h>i|j"), prefix + "a_b_c_d_e_f_g_h_i_j");
	EXPECT_EQ(smulti::clip_file_stem(t, std::string("tab\there\x01") + "x"), prefix + "tab_here_x");
	EXPECT_EQ(smulti::clip_file_stem(t, "Scene. . "), prefix + "Scene");
	EXPECT_EQ(smulti::clip_file_stem(t, " .. "), prefix + "vertical");
	/* cut to 80 bytes */
	const std::string long_name(200, 'x');
	EXPECT_EQ(smulti::clip_file_stem(t, long_name), prefix + std::string(80, 'x'));
	/* never inside a UTF-8 sequence: 79 x + "\xC3\xA4" (a-umlaut) keeps 79 x */
	EXPECT_EQ(smulti::clip_file_stem(t, std::string(79, 'x') + "\xC3\xA4"), prefix + std::string(79, 'x'));
	/* a cut that leaves a trailing space is trimmed again */
	EXPECT_EQ(smulti::clip_file_stem(t, std::string(79, 'x') + "  tail"), prefix + std::string(79, 'x'));
	/* non-ASCII names are kept */
	EXPECT_EQ(smulti::clip_file_stem(t, "Kapelle \xC3\xA4"), prefix + "Kapelle \xC3\xA4");
}

SMULTI_TEST(vertical_clip_path_avoids_existing_files)
{
	std::set<std::string> existing;
	auto exists = [&existing](const std::string &p) { return existing.count(p) > 0; };
	const std::string stem = "2026-10-19 07-05-09-V Gaming";
	EXPECT_EQ(smulti::vertical_record_file_path("E:/Clips/vertical/", stem, exists, ".mp4"),
		  "E:/Clips/vertical/2026-10-19 07-05-09-V Gaming.mp4");
	existing.insert("E:/Clips/vertical/2026-10-19 07-05-09-V Gaming.mp4");
	EXPECT_EQ(smulti::vertical_record_file_path("E:/Clips/vertical", stem, exists, ".mp4"),
		  "E:/Clips/vertical/2026-10-19 07-05-09-V Gaming (2).mp4");
	existing.insert("E:/Clips/vertical/2026-10-19 07-05-09-V Gaming (2).mp4");
	EXPECT_EQ(smulti::vertical_record_file_path("E:/Clips/vertical", stem, exists, ".mp4"),
		  "E:/Clips/vertical/2026-10-19 07-05-09-V Gaming (3).mp4");
	/* the candidate names the non-replacing clip move walks through */
	EXPECT_EQ(smulti::vertical_record_numbered_path("E:/c/", "s", 1, ".mp4"), "E:/c/s.mp4");
	EXPECT_EQ(smulti::vertical_record_numbered_path("E:/c", "s", 7, ".mp4"), "E:/c/s (7).mp4");
	/* the recording keeps its MKV default */
	EXPECT_EQ(smulti::vertical_record_file_path("E:/R", "s", exists), "E:/R/s.mkv");
}
