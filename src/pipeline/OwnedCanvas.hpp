/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/SceneLink.hpp"

#include <mutex>
#include <string>

// libobs forward declarations
struct obs_canvas;
typedef struct obs_canvas obs_canvas_t;
struct obs_data;
typedef struct obs_data obs_data_t;
struct calldata;
typedef struct calldata calldata_t;
struct obs_source;
typedef struct obs_source obs_source_t;
struct obs_weak_canvas;
typedef struct obs_weak_canvas obs_weak_canvas_t;

namespace smulti {

class EndpointRegistry;

/**
 * OBS adapter for the plugin-owned canvas "StreamMulticast Vertical".
 *
 * The create/reuse/reset decision is the pure decide_owned_canvas(); this
 * class only applies it. ensure() and on_collection_changing() run on the UI
 * thread (frontend event callbacks); get_ref()/uuid() are mutex guarded.
 *
 * Only the canvas named kOwnedVerticalCanvasName is ever touched: other
 * canvases (Aitum Vertical) and their scenes are never written.
 *
 * The canvas is held through a weak reference, so this class never extends
 * its lifetime; get_ref() hands out a strong reference per use.
 */
class OwnedCanvas {
public:
	OwnedCanvas() = default;
	~OwnedCanvas();
	OwnedCanvas(const OwnedCanvas &) = delete;
	OwnedCanvas &operator=(const OwnedCanvas &) = delete;

	/**
	 * Create, reuse or reset the owned canvas, repair items with an unusable scale_ref (rebased to the
	 * canvas size, one log line per repaired scene), then make sure it has a scene. UI thread only.
	 */
	void ensure();

	/** Scene collection is about to change: stop endpoints bound to the canvas, drop our reference. */
	void on_collection_changing(EndpointRegistry &registry);

	/**
	 * Frontend save/load hook. libobs persists no channel assignment for a
	 * non-main canvas, so the name of the scene on channel 0 is stored in the
	 * collection data and restored by ensure().
	 */
	void on_save_load(obs_data_t *data, bool saving);

	/** Drop the reference without stopping anything (OBS exit, module unload). */
	void release();

	/** Strong reference (caller releases with obs_canvas_release) or nullptr. */
	obs_canvas_t *get_ref() const;

	/** UUID of the owned canvas, empty when there is none. */
	std::string uuid() const;

	/* ---- linked scene switching (main program scene -> channel 0 of the owned canvas) ---- */

	/** Copy of the link settings (mutex guarded, any thread). */
	SceneLinkSettings link_settings() const;

	/** Replace the link settings (any thread). Persisted by the next save callback. */
	void set_link_settings(const SceneLinkSettings &settings);

	/** A scene (main or vertical) was renamed: migrate explicit entries. Any thread. */
	void on_scene_renamed(const std::string &old_name, const std::string &new_name);

	/**
	 * Switch channel 0 of the owned canvas to the scene linked to the current program scene.
	 * UI thread only. Takes and releases every reference inside the call. No-op when the link is
	 * disabled, nothing matches, the canvas is missing or the linked scene is already active.
	 */
	void follow_program_scene();

	/** Same as follow_program_scene() but for an explicit main scene name (delayed stinger switch). UI thread only. */
	void follow_scene(const std::string &main_scene);

	/**
	 * Milliseconds after transition_start at which the vertical switch should happen: 0 (simultaneous)
	 * unless the link setting "wait for stinger transition point" is on and `transition` is a stinger,
	 * then its transition point. Any thread (reads source settings and the guarded link settings only).
	 */
	int switch_delay_ms(obs_source_t *transition) const;

private:
	void remember(obs_canvas_t *canvas);
	void disconnect_rename(obs_weak_canvas_t *weak);
	static void on_canvas_rename(void *param, calldata_t *cd);
	void ensure_active_scene(obs_canvas_t *canvas);

	mutable std::mutex m_mutex;
	obs_weak_canvas_t *m_weak = nullptr;
	std::string m_uuid;
	std::string m_scene_name; ///< scene wanted on channel 0 (loaded from or saved to the collection)
	SceneLinkSettings m_links; ///< loaded from / saved to the collection (guarded by m_mutex)
};

} // namespace smulti
