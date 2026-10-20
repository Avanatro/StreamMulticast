/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/SceneLink.hpp"
#include "../core/VerticalTransition.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

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
struct obs_weak_source;
typedef struct obs_weak_source obs_weak_source_t;

namespace smulti {

class EndpointRegistry;

/** How OwnedCanvas::switch_scene changes the active vertical scene. */
enum class SceneSwitch {
	Instant, ///< obs_transition_set: restore, scene removal hand-off, catch-up after a main switch
	Manual,  ///< "Set active" in the dock: the vertical transition setting (follow main / cut / custom)
};

/**
 * What the libobs source_transition_start handler captured from the main transition (any thread), handed
 * to the UI thread as plain values: no libobs reference crosses the queue.
 */
struct MainTransitionStart {
	std::string uuid;          ///< the frontend transition's uuid (key of its prepared vertical transition)
	std::string id;            ///< transition type id
	std::string name;          ///< transition name (frontend)
	std::string settings_json; ///< its settings
	std::string dest_scene;    ///< destination main scene name
	std::string override_name; ///< the destination scene's per-scene override transition ("" none)
	int64_t override_duration_ms = 0;
};

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
 *
 * Channel 0 holds a plugin-owned private transition ("StreamMulticast Vertical Transition", canvas base
 * size, centre alignment, muted) whose active child is the vertical scene (TASK-31). Every reader of "the
 * active vertical scene" goes through active_scene_ref() (the destination while a transition runs); every
 * switch goes through switch_scene() / follow_scene() / on_main_transition_start().
 *
 * Frontend transitions are private sources, so libobs never emits the GLOBAL source_transition_start for
 * them (obs_source_dosignal skips obs->signals for private sources): the main transition start is taken
 * from each frontend transition's own "transition_start" signal, connected with its "update" signal
 * (fix round 3, live defect).
 *
 * Transitions are PREPARED ahead of use (fix round 1): one vertical transition per frontend transition,
 * keyed by its uuid, created and refreshed on ensure(), TRANSITION_LIST_CHANGED / TRANSITION_CHANGED, each
 * frontend transition's "update" signal and after each vertical run. A stinger loads its media
 * asynchronously, so the start path never creates or updates one: it only puts the prepared transition on
 * channel 0 and starts it; with nothing prepared it fades (the base transition) for that one switch.
 * All transition references are UI-thread state, released after channel 0 is cleared by
 * release_transition() (canvas removal, collection change, exit, unload; shutdown_transitions() at
 * scripting shutdown also stops any later re-creation).
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

	/**
	 * True while the owned canvas exists but has no video mix, or runs at an fps other than the main fps
	 * (a main FPS change: libobs rebuilds the mix from the canvas's stored video info), because ensure()'s
	 * obs_canvas_reset_video was refused while OBS video was active. plugin-main calls ensure() again when a
	 * main output stops. UI thread.
	 */
	bool needs_video() const;

	/* ---- linked scene switching (main program scene -> channel 0 of the owned canvas) ---- */

	/** Copy of the link settings (mutex guarded, any thread). */
	SceneLinkSettings link_settings() const;

	/** Replace the link settings (any thread). Persisted by the next save callback. */
	void set_link_settings(const SceneLinkSettings &settings);

	/** A scene (main or vertical) was renamed: migrate explicit entries. Any thread. */
	void on_scene_renamed(const std::string &old_name, const std::string &new_name);

	/**
	 * Make the scene linked to the current program scene the active vertical scene, instantly (catch-up:
	 * collection load, SCENE_CHANGED after a cut or a missed transition_start). UI thread only. Takes and
	 * releases every reference inside the call. No-op when the link is disabled, nothing matches, the canvas
	 * is missing or the linked scene is already active (or already the destination of a running transition).
	 */
	void follow_program_scene();

	/** Same as follow_program_scene() but for an explicit main scene name. UI thread only. */
	void follow_scene(const std::string &main_scene);

	/* ---- vertical scene transition (TASK-31) ---- */

	/**
	 * Strong ref to the active vertical scene of `canvas` (caller releases with obs_source_release) or null.
	 * Unwraps the channel 0 transition; while it runs, the DESTINATION scene. The only reader of channel 0.
	 */
	static obs_source_t *active_scene_ref(obs_canvas_t *canvas);

	/** Name of active_scene_ref(canvas), "" when none. */
	static std::string active_scene_name(obs_canvas_t *canvas);

	/**
	 * Make `scene` (a scene of the owned canvas `canvas`) the active vertical scene. UI thread only.
	 * False when `canvas` is not the owned canvas.
	 */
	bool switch_scene(obs_canvas_t *canvas, obs_source_t *scene, SceneSwitch how);

	/**
	 * The main transition on output channel 0 just started (queued from source_transition_start): start the
	 * vertical transition to the linked scene now (follow main: same type, settings copy, duration). UI thread.
	 */
	void on_main_transition_start(const MainTransitionStart &start);

	/**
	 * Create or refresh the prepared vertical transition of every frontend transition (type and settings),
	 * so a stinger is loaded before it plays; a running one is refreshed after it stops. UI thread only.
	 */
	void prepare_transition();

	/** Clear channel 0 (while the canvas lives) and release every vertical transition. UI thread, idempotent. */
	void release_transition();

	/** Scripting shutdown: release_transition() and never create a transition again. UI thread. */
	void shutdown_transitions();

	/** Vertical transition settings (mutex guarded, any thread). Persisted by the next save callback. */
	VerticalTransitionSettings transition_settings() const;
	void set_transition_settings(const VerticalTransitionSettings &settings);

	/** Called on the UI thread after a switch changed the active vertical scene (the channel does not change). */
	void set_active_scene_listener(std::function<void()> listener);

	/** Runs a function on the UI thread later (plugin-main's post_to_ui). Set once at load, before ensure(). */
	using UiPoster = std::function<void(std::function<void()>)>;
	void set_ui_poster(UiPoster post) { m_post = std::move(post); }

private:
	/** Prepared vertical transition of one frontend transition. */
	struct Prepared {
		std::string uuid;                     ///< frontend transition uuid
		std::string name;                     ///< frontend transition name (custom mode lookup)
		obs_weak_source_t *frontend = nullptr; ///< for disconnecting its "update" signal
		VerticalTransitionPlan plan;          ///< what this frontend transition means on the vertical canvas
		obs_source_t *wrapper = nullptr;      ///< own transition (null: instant or the base fade)
		std::string wrapper_id;
		std::string applied_json; ///< settings applied to `wrapper`
		bool warned = false;      ///< fade-fallback warning logged
	};

	void remember(obs_canvas_t *canvas);
	void disconnect_canvas_signals(obs_weak_canvas_t *weak);
	static void on_canvas_rename(void *param, calldata_t *cd);
	static void on_canvas_source_remove(void *param, calldata_t *cd);
	void repair_removed_scene();
	void ensure_active_scene(obs_canvas_t *canvas);
	bool is_owned(obs_canvas_t *canvas) const;

	void refresh_prepared_on(obs_canvas_t *canvas);
	obs_source_t *create_wrapper(obs_canvas_t *canvas, const std::string &id);
	void destroy_wrapper(obs_source_t *wrapper);
	void retire_wrapper(obs_canvas_t *canvas, obs_source_t *wrapper);
	bool is_own_wrapper(obs_source_t *source) const;
	obs_source_t *ensure_base(obs_canvas_t *canvas);
	obs_source_t *channel_wrapper(obs_canvas_t *canvas);
	void install(obs_canvas_t *canvas, obs_source_t *wrapper);
	const Prepared *find_prepared(const std::string &uuid, const std::string &name) const;
	void start_switch(obs_canvas_t *canvas, obs_source_t *scene, const Prepared *entry, bool instant,
			  uint32_t main_duration_ms);
	void queue_refresh();
	void disconnect_frontend(obs_weak_source_t *frontend);
	static void on_transition_stop(void *param, calldata_t *cd);
	static void on_frontend_transition_start(void *param, calldata_t *cd);
	static void on_frontend_update(void *param, calldata_t *cd);

	mutable std::mutex m_mutex;
	obs_weak_canvas_t *m_weak = nullptr;
	std::string m_uuid;
	std::string m_scene_name; ///< scene wanted on channel 0 (loaded from or saved to the collection)
	SceneLinkSettings m_links; ///< loaded from / saved to the collection (guarded by m_mutex)
	VerticalTransitionSettings m_transition_settings; ///< loaded from / saved to the collection (m_mutex)

	/* UI thread only. */
	std::vector<Prepared> m_prepared;
	obs_source_t *m_base = nullptr; ///< fade: first content of channel 0, fallback and fade-fallback plans
	std::function<void()> m_active_listener;

	UiPoster m_post; ///< written once at load; read by the signal callbacks (any thread)
	std::atomic<bool> m_shutting_down{false};
	std::atomic<bool> m_refresh_queued{false};
};

} // namespace smulti
