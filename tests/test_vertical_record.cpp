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
