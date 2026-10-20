/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/VerticalRecord.hpp"

#include <cstddef>
#include <functional>
#include <string>

// libobs forward declarations
struct obs_output;
typedef struct obs_output obs_output_t;
struct obs_encoder;
typedef struct obs_encoder obs_encoder_t;
struct obs_canvas;
typedef struct obs_canvas obs_canvas_t;
struct video_output;
typedef struct video_output video_t;

namespace smulti {

class ControllerReaper;
class EncoderFactory;
class OwnedCanvas;

/**
 * Shared parts of the file outputs on the owned vertical canvas (VerticalRecorder, VerticalReplayBuffer):
 * canvas ref + video, the encoder pair, the NVENC -> x264 start retry, the folder check, the teardown job
 * and the non-replacing move of a finished file. Each output class keeps its own libobs output and settings.
 * Every function runs on the UI thread except the move (any thread).
 */

/** Audio of every vertical file output: AAC 160 kbps (OBS recording default) from the main mix, track 1. */
inline constexpr int kVerticalAudioBitrateKbps = 160;
inline constexpr size_t kVerticalAudioMixTrack1 = 0;

/** Handles of one output session. The canvas ref is what keeps the encoder's video_t alive. */
struct VerticalSession {
	obs_output_t *output = nullptr;
	obs_encoder_t *video = nullptr;
	obs_encoder_t *audio = nullptr;
	obs_canvas_t *canvas = nullptr;
};

struct VerticalCanvasVideo {
	obs_canvas_t *canvas = nullptr; ///< strong ref, the caller's (null on failure)
	video_t *video = nullptr;
	std::string error; ///< why not ("The vertical canvas ... ."), without the caller's "Not ..." suffix
};

/**
 * Strong ref of the owned canvas plus its video_t. When the canvas has no video yet the ref is reaped
 * (released on the reaper, never on the UI thread's draw) and `error` is set.
 */
VerticalCanvasVideo acquire_vertical_canvas_video(OwnedCanvas &owned, ControllerReaper &reaper);

/** os_mkdirs(folder). False with `error` ("Could not create the folder \"...\".") when it fails. */
bool ensure_vertical_folder(const std::string &folder, std::string &error);

/** Video encoder (create_record_video_encoder on `video`) and AAC track 1 into s.video / s.audio. */
void create_vertical_encoders(const EncoderFactory &factory, const RecordEncoderChoice &choice,
			      const std::string &output_name, video_t *video, VerticalSession &s);

/**
 * try_start(choice, error) once; when it fails with an NVENC id, logs "<log_prefix>: <error> with <id>,
 * retrying with ..." and tries vertical_record_fallback_encoder (x264 CRF 20) once more. `choice` is the
 * one that ran (or failed last).
 */
bool start_with_encoder_fallback(const char *log_prefix, RecordEncoderChoice &choice, std::string &error,
				 const std::function<bool(const RecordEncoderChoice &, std::string &)> &try_start);

/**
 * Release the session on the ControllerReaper (never the UI or graphics thread): the output (force-stopped
 * first when `force`), the encoders, and the canvas ref LAST.
 */
void reap_vertical_session(ControllerReaper &reaper, const VerticalSession &s, bool force);

/**
 * Moves `from` to "<folder>/<stem><extension>", or the first free " (N)" variant, never replacing an
 * existing file (a target that appears between the check and the move makes it take the next number).
 * Returns the final path, or "" when the move failed (`from` is kept). Any thread.
 */
std::string move_to_free_name(const std::string &from, const std::string &folder, const std::string &stem,
			      const std::string &extension);

} // namespace smulti
