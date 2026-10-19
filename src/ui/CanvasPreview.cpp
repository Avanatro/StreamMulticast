/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "CanvasPreview.hpp"

#include "../core/LayoutMath.hpp"
#include "../pipeline/OwnedCanvas.hpp"

#include <obs.h>
#include <graphics/graphics.h>

#include <QtGui/QWindow>

#include <cmath>

namespace smulti {

namespace {

constexpr uint32_t kFallbackWidth = 1080;
constexpr uint32_t kFallbackHeight = 1920;
constexpr uint32_t kBackgroundColor = 0xFF1A1A1A; // opaque dark grey, also the letterbox bars

QSize pixel_size(const QWidget *w)
{
	return w->size() * w->devicePixelRatioF();
}

} // namespace

CanvasPreview::CanvasPreview(OwnedCanvas &canvas, QWidget *parent) : QWidget(parent), m_canvas(canvas)
{
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMinimumSize(160, 160);
}

CanvasPreview::~CanvasPreview()
{
	destroy_display();
}

void CanvasPreview::shutdown()
{
	m_shutdown = true;
	destroy_display();
}

void CanvasPreview::create_display()
{
#ifdef _WIN32
	if (m_display || m_shutdown)
		return;

	const WId id = winId(); // forces the native window to exist
	if (!id || !windowHandle())
		return;

	const QSize size = pixel_size(this);
	if (size.width() <= 0 || size.height() <= 0)
		return;

	gs_init_data info = {};
	info.cx = static_cast<uint32_t>(size.width());
	info.cy = static_cast<uint32_t>(size.height());
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	info.window.hwnd = reinterpret_cast<void *>(id);

	m_display = obs_display_create(&info, kBackgroundColor);
	if (m_display)
		obs_display_add_draw_callback(m_display, &CanvasPreview::draw, this);
#endif
}

void CanvasPreview::destroy_display()
{
	if (!m_display)
		return;
	/* obs_display_destroy takes the callback lock, so a draw in flight has
	 * finished (and dropped its canvas ref) when it returns. */
	obs_display_remove_draw_callback(m_display, &CanvasPreview::draw, this);
	obs_display_destroy(m_display);
	m_display = nullptr;
}

void CanvasPreview::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	create_display();
}

void CanvasPreview::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (!m_display) {
		create_display();
		return;
	}
	const QSize size = pixel_size(this);
	if (size.width() > 0 && size.height() > 0)
		obs_display_resize(m_display, static_cast<uint32_t>(size.width()),
				   static_cast<uint32_t>(size.height()));
}

void CanvasPreview::paintEvent(QPaintEvent *event)
{
	create_display();
	QWidget::paintEvent(event);
}

/* Graphics thread. Touches only m_canvas (mutex guarded get_ref) and libobs. */
void CanvasPreview::draw(void *param, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<CanvasPreview *>(param);
	if (!self || cx == 0 || cy == 0)
		return;

	obs_canvas_t *canvas = self->m_canvas.get_ref();
	if (!canvas)
		return; // no canvas yet, or removed: the display shows its background colour

	if (obs_canvas_has_video(canvas)) {
		uint32_t cw = kFallbackWidth;
		uint32_t ch = kFallbackHeight;
		obs_video_info ovi = {};
		if (obs_canvas_get_video_info(canvas, &ovi) && ovi.base_width > 0 && ovi.base_height > 0) {
			cw = ovi.base_width;
			ch = ovi.base_height;
		}

		const layout::ItemTransform fit = layout::fit({cw, ch}, {cx, cy});
		const int vx = static_cast<int>(std::lround(fit.pos_x));
		const int vy = static_cast<int>(std::lround(fit.pos_y));
		const int vw = static_cast<int>(std::lround(cw * fit.scale_x));
		const int vh = static_cast<int>(std::lround(ch * fit.scale_y));

		if (vw > 0 && vh > 0) {
			gs_viewport_push();
			gs_projection_push();
			gs_ortho(0.0f, static_cast<float>(cw), 0.0f, static_cast<float>(ch), -100.0f, 100.0f);
			gs_set_viewport(vx, vy, vw, vh);
			obs_canvas_render(canvas);
			gs_projection_pop();
			gs_viewport_pop();
		}
	}

	obs_canvas_release(canvas);
}

} // namespace smulti
