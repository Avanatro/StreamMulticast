/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "VerticalEncodeSession.hpp"

#include "ControllerReaper.hpp"
#include "EncoderFactory.hpp"
#include "OwnedCanvas.hpp"
#include "../plugin-support.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <optional>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace smulti {

namespace {

enum class MoveResult { Moved, TargetExists, Failed };

/** Rename that fails instead of replacing an existing target (os_rename replaces: MOVEFILE_REPLACE_EXISTING). */
MoveResult move_no_replace(const std::string &from, const std::string &to)
{
#ifdef _WIN32
	wchar_t *wfrom = nullptr;
	wchar_t *wto = nullptr;
	os_utf8_to_wcs_ptr(from.c_str(), 0, &wfrom);
	os_utf8_to_wcs_ptr(to.c_str(), 0, &wto);
	MoveResult result = MoveResult::Failed;
	if (wfrom && wto) {
		if (MoveFileExW(wfrom, wto, MOVEFILE_COPY_ALLOWED)) { // no MOVEFILE_REPLACE_EXISTING
			result = MoveResult::Moved;
		} else {
			const DWORD err = GetLastError();
			if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS)
				result = MoveResult::TargetExists;
		}
	}
	bfree(wfrom);
	bfree(wto);
	return result;
#else
	if (os_file_exists(to.c_str()))
		return MoveResult::TargetExists;
	return os_rename(from.c_str(), to.c_str()) == 0 ? MoveResult::Moved : MoveResult::Failed;
#endif
}

} // namespace

VerticalCanvasVideo acquire_vertical_canvas_video(OwnedCanvas &owned, ControllerReaper &reaper)
{
	VerticalCanvasVideo out;
	obs_canvas_t *canvas = owned.get_ref();
	if (!canvas) {
		out.error = "The vertical canvas is not available.";
		return out;
	}
	video_t *video = obs_canvas_get_video(canvas);
	if (!video) {
		VerticalSession only_canvas;
		only_canvas.canvas = canvas;
		reap_vertical_session(reaper, only_canvas, false);
		out.error = "The vertical canvas has no video yet.";
		return out;
	}
	out.canvas = canvas;
	out.video = video;
	return out;
}

bool ensure_vertical_folder(const std::string &folder, std::string &error)
{
	if (os_mkdirs(folder.c_str()) != MKDIR_ERROR)
		return true;
	error = "Could not create the folder \"" + folder + "\".";
	return false;
}

void create_vertical_encoders(const EncoderFactory &factory, const RecordEncoderChoice &choice,
			      const std::string &output_name, video_t *video, VerticalSession &s)
{
	s.video = factory.create_record_video_encoder(choice, output_name + " video", video);
	s.audio = factory.create_aac_encoder(output_name + " audio", kVerticalAudioBitrateKbps, kVerticalAudioMixTrack1);
}

bool start_with_encoder_fallback(const char *log_prefix, RecordEncoderChoice &choice, std::string &error,
				 const std::function<bool(const RecordEncoderChoice &, std::string &)> &try_start)
{
	if (try_start(choice, error))
		return true;
	/* NVENC can be registered and still fail (session limit, init error): retry once with x264, so an output
	 * started together with a main one never silently produces no vertical file. */
	const std::optional<RecordEncoderChoice> fallback =
		vertical_record_fallback_encoder(choice.type_id, EncoderFactory::available_encoder_types());
	if (!fallback)
		return false;
	obs_log(LOG_WARNING, "%s: %s with %s, retrying with %s %s %d", log_prefix, error.c_str(), choice.type_id.c_str(),
		fallback->type_id.c_str(), fallback->rate_control.c_str(), fallback->quality);
	choice = *fallback;
	return try_start(choice, error);
}

void reap_vertical_session(ControllerReaper &reaper, const VerticalSession &s, bool force)
{
	if (!s.output && !s.video && !s.audio && !s.canvas)
		return;
	/* Off the UI thread, and never on the graphics thread: the canvas ref is released LAST, after the
	 * encoders that read its video_t and the output that holds them. */
	reaper.enqueue([s, force]() {
		if (s.output) {
			if (force)
				obs_output_force_stop(s.output);
			obs_output_release(s.output);
		}
		if (s.video)
			obs_encoder_release(s.video);
		if (s.audio)
			obs_encoder_release(s.audio);
		if (s.canvas)
			obs_canvas_release(s.canvas);
	});
}

std::string move_to_free_name(const std::string &from, const std::string &folder, const std::string &stem,
			      const std::string &extension)
{
	constexpr int kMaxNumber = 10000;
	for (int n = 1; n < kMaxNumber; ++n) {
		const std::string target = vertical_record_numbered_path(folder, stem, n, extension);
		if (os_file_exists(target.c_str()))
			continue;
		switch (move_no_replace(from, target)) {
		case MoveResult::Moved:
			return target;
		case MoveResult::TargetExists:
			continue; // created between the check and the move: take the next number
		case MoveResult::Failed:
			return "";
		}
	}
	return "";
}

} // namespace smulti
