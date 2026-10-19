/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <QtWidgets/QWidget>

#include <atomic>

class QCheckBox;
class QLabel;
class QShowEvent;
class QTableWidget;

namespace smulti {

class OwnedCanvas;

/**
 * "Links" tab of the vertical dock: which vertical scene follows which main
 * scene. Edits go to OwnedCanvas::set_link_settings (persisted with the scene
 * collection); the pure resolution lives in core/SceneLink.
 *
 * Reference rules: this widget connects no libobs signals and keeps no libobs
 * reference. refresh() takes and releases the canvas ref and the main scene
 * list inside one UI-thread call. It runs on show, on frontend scene list
 * changes and when the Scenes tab changed something (VerticalCanvasDock).
 * After shutdown() every entry point returns early.
 */
class VerticalSceneLink : public QWidget {
	Q_OBJECT

public:
	explicit VerticalSceneLink(OwnedCanvas &canvas, QWidget *parent = nullptr);
	~VerticalSceneLink() override;

	/** Rebuild the table from the main scene list, the vertical scenes and the saved links. UI thread only. */
	void refresh();

	/** Never touch libobs again (OBS exit, module unload). Idempotent. */
	void shutdown();

protected:
	void showEvent(QShowEvent *event) override;

private slots:
	void on_follow_toggled(bool checked);
	void on_wait_stinger_toggled(bool checked);
	void on_choice_changed(const QString &main_scene, int kind, const QString &vertical_scene);

private:
	OwnedCanvas &m_canvas;
	QCheckBox *m_follow = nullptr;
	QCheckBox *m_wait_stinger = nullptr;
	QTableWidget *m_table = nullptr;
	QLabel *m_status = nullptr;
	bool m_refreshing = false;
	std::atomic<bool> m_shutdown{false};
};

} // namespace smulti
