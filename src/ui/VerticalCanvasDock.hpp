/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <QtCore/QObject>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QWidget>

#include <cstdint>
#include <string>

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
 * The one selected scene item of the vertical dock (G2: single source of truth). The Sources list and
 * the preview both read and write it; neither keeps its own copy. Item ids are unique per scene only,
 * so the selection always carries the uuid of the scene the id belongs to. UI thread only; holds no
 * libobs reference. changed() fires only when the value really changes.
 */
class VerticalSelection : public QObject {
	Q_OBJECT

public:
	explicit VerticalSelection(QObject *parent = nullptr) : QObject(parent) {}

	void set(const std::string &scene_uuid, int64_t item_id)
	{
		if (item_id == 0 || scene_uuid.empty()) {
			clear();
			return;
		}
		if (scene_uuid == m_scene_uuid && item_id == m_item_id)
			return;
		m_scene_uuid = scene_uuid;
		m_item_id = item_id;
		emit changed();
	}
	void clear()
	{
		if (m_item_id == 0 && m_scene_uuid.empty())
			return;
		m_scene_uuid.clear();
		m_item_id = 0;
		emit changed();
	}
	const std::string &scene_uuid() const { return m_scene_uuid; }
	int64_t item_id() const { return m_item_id; }
	/** The selected id if it belongs to scene_uuid, else 0. */
	int64_t item_in(const std::string &scene_uuid) const
	{
		return !scene_uuid.empty() && scene_uuid == m_scene_uuid ? m_item_id : 0;
	}

signals:
	void changed();

private:
	std::string m_scene_uuid;
	int64_t m_item_id = 0;
};

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
	VerticalSelection *m_selection = nullptr; ///< child; re-parented last so it is deleted after the widgets
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
