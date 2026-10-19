/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <QtWidgets/QWidget>

#include <cstdint>

// libobs forward declaration
struct obs_display;
typedef struct obs_display obs_display_t;

namespace smulti {

class OwnedCanvas;

/**
 * Live preview of the plugin-owned vertical canvas.
 *
 * A native child window carries an obs_display; its draw callback (graphics
 * thread) takes a strong canvas reference from OwnedCanvas::get_ref() every
 * frame, renders the canvas letterboxed into the widget and releases the
 * reference again. It renders nothing while the canvas is absent or removed,
 * so a collection switch or video reset needs no explicit handling here.
 *
 * Lifetime rule: the display is destroyed (which waits for a running draw
 * callback) before the widget is destroyed and before the OwnedCanvas goes
 * away. shutdown() does that early (OBS exit, module unload); the destructor
 * repeats it.
 */
class CanvasPreview : public QWidget {
	Q_OBJECT

public:
	explicit CanvasPreview(OwnedCanvas &canvas, QWidget *parent = nullptr);
	~CanvasPreview() override;

	/** Destroy the display now and never create it again. UI thread; idempotent. */
	void shutdown();

	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;

private:
	void create_display();
	void destroy_display();
	static void draw(void *param, uint32_t cx, uint32_t cy);

	OwnedCanvas &m_canvas;
	obs_display_t *m_display = nullptr;
	bool m_shutdown = false;
};

} // namespace smulti
