/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <mutex>
#include <string>

// libobs forward declarations
struct obs_canvas;
typedef struct obs_canvas obs_canvas_t;
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

	/** Create, reuse or reset the owned canvas, then make sure it has a scene. UI thread only. */
	void ensure();

	/** Scene collection is about to change: stop endpoints bound to the canvas, drop our reference. */
	void on_collection_changing(EndpointRegistry &registry);

	/** Drop the reference without stopping anything (OBS exit, module unload). */
	void release();

	/** Strong reference (caller releases with obs_canvas_release) or nullptr. */
	obs_canvas_t *get_ref() const;

	/** UUID of the owned canvas, empty when there is none. */
	std::string uuid() const;

private:
	void remember(obs_canvas_t *canvas);
	void ensure_default_scene(obs_canvas_t *canvas);

	mutable std::mutex m_mutex;
	obs_weak_canvas_t *m_weak = nullptr;
	std::string m_uuid;
};

} // namespace smulti
