/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <QtWidgets/QTabWidget>
#include <QtWidgets/QWidget>

namespace smulti {

class CanvasPreview;
class OwnedCanvas;

/**
 * Second dock ("StreamMulticast Vertical", id avanatro_streammulticast_vertical):
 * live preview of the owned vertical canvas on top, a tab area below that later
 * tasks fill with the scene list and source controls.
 */
class VerticalCanvasDock : public QWidget {
	Q_OBJECT

public:
	explicit VerticalCanvasDock(OwnedCanvas &canvas, QWidget *parent = nullptr);
	~VerticalCanvasDock() override = default;

	/** Tear the preview display down before the canvas or the dock go away. */
	void shutdown_preview();

	/** Barrier against an in-flight preview draw (see CanvasPreview::flush). */
	void flush_preview();

private:
	CanvasPreview *m_preview = nullptr;
	QTabWidget *m_tabs = nullptr;
};

} // namespace smulti
