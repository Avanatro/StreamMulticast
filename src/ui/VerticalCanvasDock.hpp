/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <QtWidgets/QTabWidget>
#include <QtWidgets/QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace smulti {

class CanvasPreview;
class OwnedCanvas;
class VerticalItemList;
class VerticalSceneLink;
class VerticalSceneList;
class VerticalRecorder;

/**
 * Second dock ("StreamMulticast Vertical", id avanatro_streammulticast_vertical):
 * live preview of the owned vertical canvas on top, the "Record vertical" button,
 * and tabs for scenes, sources, links and the vertical recording settings.
 */
class VerticalCanvasDock : public QWidget {
	Q_OBJECT

public:
	VerticalCanvasDock(OwnedCanvas &canvas, VerticalRecorder &recorder, QWidget *parent = nullptr);
	~VerticalCanvasDock() override = default;

	/** Tear the preview display down before the canvas or the dock go away. */
	void shutdown_preview();

	/** Barrier against an in-flight preview draw (see CanvasPreview::flush). */
	void flush_preview();

	/** Rebuild the scene list (UI thread). Call after OwnedCanvas::ensure(). */
	void refresh_scenes();

	/** Rebuild the Links tab (main scene list changed). UI thread. */
	void refresh_links();

	/** Disconnect the scene list from the canvas signals; any thread. Call before the canvas is released. */
	void detach_scene_signals();

private:
	QWidget *build_record_tab();
	void update_record_ui(); ///< UI thread: button state from the recorder
	void on_record_settings_changed();

	VerticalRecorder &m_recorder;
	QPushButton *m_record = nullptr;
	QCheckBox *m_record_with_main = nullptr;
	QLineEdit *m_record_folder = nullptr;
	QLabel *m_record_target = nullptr;
	CanvasPreview *m_preview = nullptr;
	VerticalSceneList *m_scenes = nullptr;
	VerticalItemList *m_items = nullptr;
	VerticalSceneLink *m_links = nullptr;
	QTabWidget *m_tabs = nullptr;
};

} // namespace smulti
