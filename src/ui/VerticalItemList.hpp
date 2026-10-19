/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "TransformEditor.hpp"

#include <QtCore/QElapsedTimer>
#include <QtWidgets/QWidget>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

// libobs forward declarations
struct obs_weak_canvas;
typedef struct obs_weak_canvas obs_weak_canvas_t;
struct obs_weak_source;
typedef struct obs_weak_source obs_weak_source_t;
struct obs_canvas;
typedef struct obs_canvas obs_canvas_t;
struct obs_scene;
typedef struct obs_scene obs_scene_t;
struct obs_source;
typedef struct obs_source obs_source_t;
struct calldata;
typedef struct calldata calldata_t;

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QLabel;

namespace smulti {

class OwnedCanvas;
class VerticalSelection;

/** fn(scene, scene_uuid) runs with the owned canvas' ACTIVE scene; see VerticalItemList::with_active_scene. */
using SceneFn = std::function<void(obs_scene_t *, const std::string &scene_uuid)>;

/**
 * "Sources" tab of the vertical dock: the items of the ACTIVE scene (channel 0) of the owned canvas,
 * with add-existing-source, remove, move up/down, a visibility checkbox and the TransformEditor.
 *
 * Reference rules (same as VerticalSceneList, binding):
 *  - Every canvas, scene, source and scene item reference is taken and released inside one function
 *    on the UI thread (see with_item). None is kept as a member or across a modal dialog, a queued
 *    call or a frontend event. Items are identified by their numeric id between events.
 *  - The long-lived links are weak references (canvas, active scene source) plus connected signals:
 *    canvas channel_change / source_remove / source_rename and the scene signals item_add,
 *    item_remove, reorder, item_visible, refresh (callbacks only queue a coalesced refresh()) and
 *    item_transform (queues only a coalesced transform panel reload; it can fire per frame).
 *  - Item ids are unique per scene only: the list remembers the scene uuid its ids came from and every
 *    libobs access passes it; a different active scene refuses the access.
 *  - detach_signals() is thread safe and must run before the canvas is released (canvas_remove,
 *    collection changing, SCRIPTING_SHUTDOWN, unload). The destructor repeats it.
 *  - Only the owned canvas is ever written (INV-6): every write re-checks that the scene belongs to
 *    the owned canvas. Added inputs are only referenced, never modified.
 *  - The selected row mirrors the dock's VerticalSelection (the single selection source, shared with
 *    the preview). A scene change or a removed item clears it.
 */
class VerticalItemList : public QWidget {
	Q_OBJECT

public:
	VerticalItemList(OwnedCanvas &canvas, VerticalSelection &selection, QWidget *parent = nullptr);
	~VerticalItemList() override;

	/** Rebuild the list and (re)connect the signals. UI thread only. */
	void refresh();

	/** Disconnect all signals. Any thread, idempotent; takes no reference that outlives the call. */
	void detach_signals();

	/** Detach and never connect or touch libobs again (OBS exit, module unload). Any thread, idempotent. */
	void shutdown();

	/**
	 * Run fn with the active scene (channel 0) of the owned canvas and its uuid, UI thread. Same reference
	 * and INV-6 rules as every other access here: all refs are released before it returns, fn must not keep
	 * the scene or any item. False (fn not called) when there is no canvas/scene or after shutdown.
	 */
	bool with_active_scene(const SceneFn &fn);

	/** Reload the transform panel now (force: also while a field has focus). UI thread. */
	void reload_editor(bool force);

	/** Called on the UI thread after the items or a transform changed (throttled like the editor reload). */
	void set_change_listener(std::function<void()> listener) { m_change_listener = std::move(listener); }

private slots:
	void on_add();
	void on_remove();
	void on_up();
	void on_down();
	void on_selection_changed();
	void on_item_changed(QListWidgetItem *item);

private:
	bool with_scene(const std::string &scene_uuid, const std::function<void(obs_scene_t *)> &fn);
	bool with_scene_impl(const std::string *required_uuid, const SceneFn &fn);
	void on_selection_model_changed();
	void editor_reload_throttled();
	void editor_reload_now();
	void notify_change();
	bool with_item(const std::string &scene_uuid, int64_t item_id, const std::function<void(obs_sceneitem_t *)> &fn);
	void connect_canvas_signals(obs_canvas_t *canvas);
	void connect_scene_signals(obs_source_t *scene_source);
	void detach_scene_signals_locked();
	void detach_canvas_signals_locked();
	void queue_refresh();
	void queue_editor_reload();
	void move_selected(bool up);
	void set_status(const QString &text);
	void update_buttons();
	int64_t selected_id() const;
	static void on_signal(void *param, calldata_t *cd);
	static void on_transform_signal(void *param, calldata_t *cd);

	OwnedCanvas &m_canvas;
	VerticalSelection &m_selection;
	std::function<void()> m_change_listener;
	QElapsedTimer m_reload_clock; ///< UI thread: last editor reload (30 Hz throttle)
	QLabel *m_scene_label = nullptr;
	QListWidget *m_list = nullptr;
	QPushButton *m_add_btn = nullptr;
	QPushButton *m_remove_btn = nullptr;
	QPushButton *m_up_btn = nullptr;
	QPushButton *m_down_btn = nullptr;
	QLabel *m_status = nullptr;
	TransformEditor *m_editor = nullptr;

	int64_t m_pending_select = 0; ///< item id to select on the next refresh (UI thread only)
	bool m_has_scene = false;     ///< UI thread only: last refresh found an active scene
	std::string m_list_scene_uuid; ///< scene the list rows (and their ids) were built from; ids are per scene
	QString m_pending_status;      ///< shown by the next refresh (UI thread only)

	std::mutex m_conn_mutex;
	obs_weak_canvas_t *m_conn_canvas = nullptr; ///< canvas the canvas signals are on (weak)
	std::string m_conn_canvas_uuid;
	obs_weak_source_t *m_conn_scene = nullptr; ///< scene source the scene signals are on (weak)
	std::string m_conn_scene_uuid;
	std::atomic<bool> m_refresh_queued{false};
	std::atomic<bool> m_reload_queued{false};
	std::atomic<bool> m_shutdown{false};
};

} // namespace smulti
