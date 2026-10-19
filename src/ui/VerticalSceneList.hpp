/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <QtWidgets/QWidget>

#include <atomic>
#include <mutex>
#include <string>

// libobs forward declarations
struct obs_weak_canvas;
typedef struct obs_weak_canvas obs_weak_canvas_t;
struct obs_canvas;
typedef struct obs_canvas obs_canvas_t;
struct calldata;
typedef struct calldata calldata_t;

class QListWidget;
class QPushButton;
class QLabel;

namespace smulti {

class OwnedCanvas;

/**
 * "Scenes" tab of the vertical dock: scenes of the plugin-owned canvas only.
 *
 * Reference rules (binding):
 *  - Every canvas, scene and source reference is taken and released inside one
 *    function on the UI thread. None is kept as a member or across a modal
 *    dialog, a queued call or a frontend event.
 *  - The only long-lived link to the canvas is a weak canvas reference plus the
 *    connected canvas signals (source_add, source_remove, source_rename,
 *    channel_change). Their callbacks only queue a coalesced refresh().
 *  - detach_signals() is thread safe and must run before the canvas is released
 *    (canvas_remove, collection changing, SCRIPTING_SHUTDOWN, unload). The
 *    destructor repeats it.
 *  - Only the owned canvas is ever written (INV-6): scene removal re-checks
 *    that the scene's canvas is the owned canvas.
 */
class VerticalSceneList : public QWidget {
	Q_OBJECT

public:
	explicit VerticalSceneList(OwnedCanvas &canvas, QWidget *parent = nullptr);
	~VerticalSceneList() override;

	/** Rebuild the list and (re)connect the canvas signals. UI thread only. */
	void refresh();

	/** Disconnect the canvas signals. Any thread, idempotent; takes no canvas ref that outlives the call. */
	void detach_signals();

	/** Detach and never connect again (OBS exit, module unload). Any thread, idempotent. */
	void shutdown();

private slots:
	void on_add();
	void on_rename();
	void on_remove();
	void on_set_active();
	void on_import();
	void update_buttons();

private:
	void connect_signals(obs_canvas_t *canvas);
	void queue_refresh();
	void set_status(const QString &text);
	QString selected_scene() const;
	static void on_canvas_signal(void *param, calldata_t *cd);

	OwnedCanvas &m_canvas;
	QListWidget *m_list = nullptr;
	QPushButton *m_add_btn = nullptr;
	QPushButton *m_rename_btn = nullptr;
	QPushButton *m_remove_btn = nullptr;
	QPushButton *m_active_btn = nullptr;
	QPushButton *m_import_btn = nullptr;
	QLabel *m_status = nullptr;

	std::mutex m_conn_mutex;
	obs_weak_canvas_t *m_conn_weak = nullptr; ///< canvas the signals are connected on (weak, never strong)
	std::string m_conn_uuid;
	std::atomic<bool> m_refresh_queued{false};
	std::atomic<bool> m_shutdown{false};
};

} // namespace smulti
