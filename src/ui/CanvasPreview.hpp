/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "../core/PreviewEdit.hpp"
#include "../core/SafeZone.hpp"

#include <QtCore/QElapsedTimer>
#include <QtWidgets/QWidget>

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// libobs forward declarations
struct obs_display;
typedef struct obs_display obs_display_t;
struct obs_scene;
typedef struct obs_scene obs_scene_t;
struct obs_scene_item;
typedef struct obs_scene_item obs_sceneitem_t;
class QContextMenuEvent;

namespace smulti {

class OwnedCanvas;
class VerticalSelection;

/** Reads a scene item into the pure editor model (UI thread, item must be held by the caller). */
preview_edit::Item read_preview_item(obs_sceneitem_t *item);

/**
 * Runs fn(scene, scene_uuid) with the owned canvas' active scene on the UI thread (all references are
 * taken and released inside the call; INV-6 checked there). False when there is no scene.
 */
using PreviewSceneAccess =
	std::function<bool(const std::function<void(obs_scene_t *, const std::string &scene_uuid)> &fn)>;

/**
 * Live preview of the plugin-owned vertical canvas, with OBS-style click-and-drag editing.
 *
 * Render: a native child window carries an obs_display; its draw callback (graphics thread) takes a
 * strong canvas reference from OwnedCanvas::get_ref() every frame, renders the canvas letterboxed into
 * the widget, draws the selection overlay and releases the reference again. It renders nothing while
 * the canvas is absent or removed, so a collection switch or video reset needs no explicit handling.
 *
 * Editing (UI thread): click selects the topmost visible, unlocked item (handles of the selected item
 * first), drag moves (snapping; Ctrl = none) or resizes (corner keeps aspect, Shift = free), arrow keys
 * nudge 1 px (Shift 10), Escape cancels a drag, Ctrl+Z undoes the last drag/nudge (one step; the OBS
 * undo stack only covers the main canvas). Delete does nothing. The selection lives in the dock's
 * VerticalSelection (shared with the Sources list); this widget keeps no copy of it.
 *
 * Threads: the graphics thread only reads cached overlay corners (canvas px) under m_overlay_mutex; it
 * never sees a scene or item pointer. Every scene/item reference is taken and released inside one UI
 * thread call through the scene access (VerticalItemList::with_active_scene); items are identified by
 * scene uuid + id between events.
 *
 * Lifetime rule: the display is destroyed (which waits for a running draw callback) before the widget is
 * destroyed and before the OwnedCanvas goes away. shutdown() does that early (OBS exit, module unload) and
 * stops all editing; the destructor repeats it.
 */
class CanvasPreview : public QWidget {
	Q_OBJECT

public:
	CanvasPreview(OwnedCanvas &canvas, VerticalSelection &selection, QWidget *parent = nullptr);
	~CanvasPreview() override;

	/** Scene access used for hit tests and writes (UI thread). Without it the preview is view-only. */
	void set_scene_access(PreviewSceneAccess access) { m_access = std::move(access); }

	/** Right click on an item (already selected): fn(global position, item id, scene uuid). UI thread. */
	void set_context_menu_handler(std::function<void(const QPoint &, int64_t, const std::string &)> handler)
	{
		m_context_handler = std::move(handler);
	}

	/** Called on the UI thread when an edit finished (drag release, Escape, undo, nudge). */
	void set_edit_listener(std::function<void()> listener) { m_edit_listener = std::move(listener); }

	/**
	 * Safe-zone overlay (preview only, drawn in draw_overlay, never rendered into the canvas) and its
	 * drag snapping. UI thread. Off by default.
	 */
	void set_safe_zone(safe_zone::Profile profile);
	safe_zone::Profile safe_zone() const { return m_safe_profile; }

	/** Re-read the selected/hovered item geometry for the overlay. UI thread. */
	void refresh_overlay();

	/** Destroy the display now and never create it again; stops editing. UI thread; idempotent. */
	void shutdown();

	/**
	 * Barrier: wait until no draw callback is running. Remove/re-add of the
	 * callback takes the same lock the display holds while drawing, so on
	 * return every canvas ref the draw took has been released. Call it after
	 * dropping OwnedCanvas's reference and before the frontend releases its own,
	 * so the final canvas release never happens on the graphics thread.
	 */
	void flush();

	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	bool event(QEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;
	void leaveEvent(QEvent *event) override;
	void focusOutEvent(QFocusEvent *event) override;

private:
	using Corners = std::array<preview_edit::Vec2, 4>;

	/** Items of the active scene, libobs order (bottom first). */
	struct Snapshot {
		std::string scene_uuid;
		std::vector<preview_edit::Item> items;
		const preview_edit::Item *find(int64_t id) const;
	};

	enum class DragMode { None, Move, Resize };
	struct Drag {
		DragMode mode = DragMode::None;
		std::string scene_uuid;
		int64_t id = 0;
		preview_edit::Item start;
		preview_edit::Handle handle = preview_edit::Handle::None;
		preview_edit::Vec2 press_canvas;
		preview_edit::Vec2 handle_start; ///< canvas position of the grabbed handle at press
		std::vector<preview_edit::Item> others;
		float snap_canvas = 0;
		bool changed = false;
	};

	struct Undo {
		bool valid = false;
		std::string scene_uuid;
		int64_t id = 0;
		preview_edit::Item before;
	};

	/** What the graphics thread draws: geometry only, canvas px. */
	struct Overlay {
		bool selected = false;
		bool selected_locked = false;
		Corners selected_corners{};
		bool hovered = false;
		Corners hovered_corners{};
		float dpr = 1.0f; ///< device px per logical px, so line/handle sizes match the hit radius
		safe_zone::Profile safe_profile = safe_zone::Profile::Off;
	};

	void create_display();
	void destroy_display();
	static void draw(void *param, uint32_t cx, uint32_t cy);
	static void draw_overlay(const Overlay &ov, layout::Size canvas, float canvas_per_px);

	bool editing_allowed() const;
	bool take_snapshot(Snapshot &out) const;
	bool write_item(const std::string &scene_uuid, const preview_edit::Item &next) const;
	layout::Size canvas_size() const;
	preview_edit::View current_view() const;
	preview_edit::Vec2 event_pos(const QMouseEvent *event) const; ///< device px, like the display
	void finish_drag(bool restore);
	void nudge_selected(int dx, int dy, bool shift);
	void undo_last();
	void update_hover(const preview_edit::Vec2 &widget_px);
	void set_selected_overlay(const preview_edit::Item *item);
	void notify_edit();

	OwnedCanvas &m_canvas;
	VerticalSelection &m_selection;
	PreviewSceneAccess m_access;
	std::function<void()> m_edit_listener;
	std::function<void(const QPoint &, int64_t, const std::string &)> m_context_handler;

	std::mutex m_display_mutex; ///< guards m_display (flush may come from a libobs signal thread)
	obs_display_t *m_display = nullptr;
	bool m_screen_connected = false;
	bool m_shutdown = false;

	/* UI thread only */
	Drag m_drag;
	Undo m_undo;
	preview_edit::NudgeBurst m_nudge_burst; ///< consecutive arrow nudges share one undo step
	QElapsedTimer m_clock;                  ///< monotonic ms for the nudge burst
	int64_t m_hover_id = 0;
	safe_zone::Profile m_safe_profile = safe_zone::Profile::Off;

	std::mutex m_overlay_mutex; ///< guards m_overlay (UI writes, graphics thread copies)
	Overlay m_overlay;
};

} // namespace smulti
