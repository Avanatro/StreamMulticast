/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "TransformEditor.hpp"

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
 *    item_remove, reorder, item_visible, item_transform, refresh. Callbacks only queue a coalesced
 *    refresh().
 *  - detach_signals() is thread safe and must run before the canvas is released (canvas_remove,
 *    collection changing, SCRIPTING_SHUTDOWN, unload). The destructor repeats it.
 *  - Only the owned canvas is ever written (INV-6): every write re-checks that the scene belongs to
 *    the owned canvas. Added inputs are only referenced, never modified.
 */
class VerticalItemList : public QWidget {
	Q_OBJECT

public:
	explicit VerticalItemList(OwnedCanvas &canvas, QWidget *parent = nullptr);
	~VerticalItemList() override;

	/** Rebuild the list and (re)connect the signals. UI thread only. */
	void refresh();

	/** Disconnect all signals. Any thread, idempotent; takes no reference that outlives the call. */
	void detach_signals();

	/** Detach and never connect or touch libobs again (OBS exit, module unload). Any thread, idempotent. */
	void shutdown();

private slots:
	void on_add();
	void on_remove();
	void on_up();
	void on_down();
	void on_selection_changed();
	void on_item_changed(QListWidgetItem *item);

private:
	bool with_scene(const std::function<void(obs_scene_t *)> &fn);
	bool with_item(int64_t item_id, const std::function<void(obs_sceneitem_t *)> &fn);
	void connect_canvas_signals(obs_canvas_t *canvas);
	void connect_scene_signals(obs_source_t *scene_source);
	void detach_scene_signals_locked();
	void detach_canvas_signals_locked();
	void queue_refresh();
	void move_selected(bool up);
	void set_status(const QString &text);
	void update_buttons();
	int64_t selected_id() const;
	static void on_signal(void *param, calldata_t *cd);

	OwnedCanvas &m_canvas;
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

	std::mutex m_conn_mutex;
	obs_weak_canvas_t *m_conn_canvas = nullptr; ///< canvas the canvas signals are on (weak)
	std::string m_conn_canvas_uuid;
	obs_weak_source_t *m_conn_scene = nullptr; ///< scene source the scene signals are on (weak)
	std::string m_conn_scene_uuid;
	std::atomic<bool> m_refresh_queued{false};
	std::atomic<bool> m_shutdown{false};
};

} // namespace smulti
