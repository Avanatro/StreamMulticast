/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "CanvasPreview.hpp"

#include "VerticalCanvasDock.hpp" // VerticalSelection
#include "../core/LayoutMath.hpp"
#include "../pipeline/OwnedCanvas.hpp"

#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#include <QtGui/QContextMenuEvent>
#include <QtGui/QFocusEvent>
#include <QtGui/QHideEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>

#include <algorithm>
#include <cmath>
#include <utility>

namespace smulti {

namespace {

constexpr uint32_t kFallbackWidth = 1080;
constexpr uint32_t kFallbackHeight = 1920;
constexpr uint32_t kBackgroundColor = 0xFF1A1A1A; // opaque dark grey, also the letterbox bars

/* Overlay sizes in logical widget px; the draw scales them by the device pixel ratio and converts them
 * to canvas units, so they match the hit radius (kHandleRadiusPx * dpr). */
constexpr float kSelectLinePx = 2.0f;
constexpr float kHoverLinePx = 1.0f;
constexpr float kHandleDrawPx = 4.0f; ///< half side of a handle square (hit radius is larger, see PreviewEdit)
constexpr float kHoverColor[3] = {0.0f, 0.5f, 1.0f};

QSize pixel_size(const QWidget *w)
{
	return w->size() * w->devicePixelRatioF();
}

} // namespace

CanvasPreview::CanvasPreview(OwnedCanvas &canvas, VerticalSelection &selection, QWidget *parent)
	: QWidget(parent),
	  m_canvas(canvas),
	  m_selection(selection)
{
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMinimumSize(160, 160);
	setMouseTracking(true); // hover outline and cursor shapes
	setFocusPolicy(Qt::ClickFocus); // arrow keys / Escape / Ctrl+Z after a click
	m_clock.start();
	connect(&m_selection, &VerticalSelection::changed, this, [this] {
		/* Another item (or none) is selected now, e.g. from the list or after a scene change: a running
		 * drag no longer belongs to the selection. Keep what was dragged, stop. */
		if (m_drag.mode != DragMode::None && m_selection.item_in(m_drag.scene_uuid) != m_drag.id)
			finish_drag(false);
		preview_edit::end_nudge_burst(m_nudge_burst);
		refresh_overlay();
	});
}

CanvasPreview::~CanvasPreview()
{
	destroy_display();
}

void CanvasPreview::shutdown()
{
	m_shutdown = true;
	m_drag = Drag{};
	m_undo = Undo{};
	preview_edit::end_nudge_burst(m_nudge_burst);
	m_hover_id = 0;
	m_access = nullptr; // never reach the scene access (and the list behind it) again
	{
		std::lock_guard<std::mutex> lock(m_overlay_mutex);
		m_overlay = Overlay{};
	}
	destroy_display();
}

void CanvasPreview::flush()
{
	std::lock_guard<std::mutex> lock(m_display_mutex);
	if (!m_display)
		return;
	/* Removal blocks on draw_callbacks_mutex, which render_display holds for the
	 * whole draw (libobs obs-display.c). */
	obs_display_remove_draw_callback(m_display, &CanvasPreview::draw, this);
	obs_display_add_draw_callback(m_display, &CanvasPreview::draw, this);
}

void CanvasPreview::create_display()
{
#ifdef _WIN32
	if (m_shutdown) // UI thread only field, same thread as shutdown()
		return;

	/* winId() creates the native window and may dispatch window messages that re-enter
	 * resizeEvent()/showEvent() and so create_display(). Take the (non-recursive) lock only after. */
	const WId id = winId(); // forces the native window to exist
	QWindow *window = windowHandle();
	if (!id || !window || !window->isExposed())
		return; // paint/resize/show retry

	const QSize size = pixel_size(this);
	if (size.width() <= 0 || size.height() <= 0)
		return;

	std::lock_guard<std::mutex> lock(m_display_mutex);
	if (m_display || m_shutdown)
		return;

	gs_init_data info = {};
	info.cx = static_cast<uint32_t>(size.width());
	info.cy = static_cast<uint32_t>(size.height());
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	info.window.hwnd = reinterpret_cast<void *>(id);

	m_display = obs_display_create(&info, kBackgroundColor);
	if (m_display) {
		obs_display_set_enabled(m_display, isVisible());
		obs_display_add_draw_callback(m_display, &CanvasPreview::draw, this);
	}

	if (!m_screen_connected) {
		m_screen_connected = true;
		connect(window, &QWindow::screenChanged, this, [this](QScreen *) {
			std::lock_guard<std::mutex> guard(m_display_mutex);
			const QSize size = pixel_size(this);
			if (m_display && size.width() > 0 && size.height() > 0)
				obs_display_resize(m_display, static_cast<uint32_t>(size.width()),
						   static_cast<uint32_t>(size.height()));
		});
	}
#endif
}

void CanvasPreview::destroy_display()
{
	std::lock_guard<std::mutex> lock(m_display_mutex);
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
	std::lock_guard<std::mutex> lock(m_display_mutex);
	if (m_display)
		obs_display_set_enabled(m_display, true);
}

void CanvasPreview::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	finish_drag(false); // the release can no longer arrive here
	std::lock_guard<std::mutex> lock(m_display_mutex);
	if (m_display)
		obs_display_set_enabled(m_display, false);
}

void CanvasPreview::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	create_display();
	std::lock_guard<std::mutex> lock(m_display_mutex);
	const QSize size = pixel_size(this);
	if (m_display && size.width() > 0 && size.height() > 0)
		obs_display_resize(m_display, static_cast<uint32_t>(size.width()),
				   static_cast<uint32_t>(size.height()));
}

void CanvasPreview::paintEvent(QPaintEvent *event)
{
	create_display();
	QWidget::paintEvent(event);
}

/* Graphics thread. Touches only m_canvas (mutex guarded get_ref), the overlay copy and libobs. */
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

		/* Same viewport as the mouse mapping (preview_edit::make_view). */
		const layout::Rect vp = layout::letterbox({cw, ch}, {cx, cy});
		if (vp.w > 0 && vp.h > 0) {
			Overlay ov;
			{
				std::lock_guard<std::mutex> lock(self->m_overlay_mutex);
				ov = self->m_overlay;
			}
			gs_viewport_push();
			gs_projection_push();
			gs_ortho(0.0f, static_cast<float>(cw), 0.0f, static_cast<float>(ch), -100.0f, 100.0f);
			gs_set_viewport(vp.x, vp.y, vp.w, vp.h);
			obs_canvas_render(canvas);

			/* Overlay over the whole display with the same canvas mapping, so handles and outlines past the
			 * canvas edge are not clipped by the letterbox viewport (like OBSBasicPreview). */
			const float upx_x = static_cast<float>(cw) / static_cast<float>(vp.w); // canvas units per device px
			const float upx_y = static_cast<float>(ch) / static_cast<float>(vp.h);
			gs_ortho(-static_cast<float>(vp.x) * upx_x, (static_cast<float>(cx) - static_cast<float>(vp.x)) * upx_x,
				 -static_cast<float>(vp.y) * upx_y, (static_cast<float>(cy) - static_cast<float>(vp.y)) * upx_y,
				 -100.0f, 100.0f);
			gs_set_viewport(0, 0, static_cast<int>(cx), static_cast<int>(cy));
			draw_overlay(ov, (upx_x + upx_y) * 0.5f * ov.dpr);
			gs_projection_pop();
			gs_viewport_pop();
		}
	}

	obs_canvas_release(canvas);
}

namespace {

using preview_edit::Vec2;

/* Thin quad from a to b, `thickness` canvas units wide, ends extended so corners close. */
void draw_line(Vec2 a, Vec2 b, float thickness)
{
	const float dx = b.x - a.x;
	const float dy = b.y - a.y;
	const float len = std::hypot(dx, dy);
	if (len < 1e-4f)
		return;
	const float h = thickness * 0.5f;
	const Vec2 d{dx / len * h, dy / len * h};
	const Vec2 n{-d.y, d.x};
	a = a - d;
	b = b + d;
	gs_render_start(true);
	gs_vertex2f(a.x + n.x, a.y + n.y);
	gs_vertex2f(a.x - n.x, a.y - n.y);
	gs_vertex2f(b.x + n.x, b.y + n.y);
	gs_vertex2f(b.x - n.x, b.y - n.y);
	gs_render_stop(GS_TRISTRIP);
}

void draw_square(Vec2 c, float half)
{
	gs_render_start(true);
	gs_vertex2f(c.x - half, c.y - half);
	gs_vertex2f(c.x + half, c.y - half);
	gs_vertex2f(c.x - half, c.y + half);
	gs_vertex2f(c.x + half, c.y + half);
	gs_render_stop(GS_TRISTRIP);
}

void draw_outline(const std::array<Vec2, 4> &c, float thickness)
{
	for (size_t i = 0; i < c.size(); ++i)
		draw_line(c[i], c[(i + 1) % c.size()], thickness);
}

Vec2 mid(Vec2 a, Vec2 b)
{
	return {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
}

} // namespace

/* Graphics thread, inside the canvas ortho: geometry only, no libobs object besides the base effect. */
void CanvasPreview::draw_overlay(const Overlay &ov, float canvas_per_px)
{
	if (!ov.selected && !ov.hovered)
		return;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = solid ? gs_effect_get_param_by_name(solid, "color") : nullptr;
	if (!solid || !color)
		return;

	vec4 c;
	if (ov.hovered) {
		vec4_set(&c, kHoverColor[0], kHoverColor[1], kHoverColor[2], 1.0f);
		gs_effect_set_vec4(color, &c);
		while (gs_effect_loop(solid, "Solid"))
			draw_outline(ov.hovered_corners, kHoverLinePx * canvas_per_px);
	}
	if (ov.selected) {
		vec4_set(&c, 1.0f, 0.0f, 0.0f, 1.0f); // OBS selection red (0xFFFF0000)
		gs_effect_set_vec4(color, &c);
		while (gs_effect_loop(solid, "Solid")) {
			const auto &k = ov.selected_corners;
			draw_outline(k, kSelectLinePx * canvas_per_px);
			if (!ov.selected_locked) {
				const float half = kHandleDrawPx * canvas_per_px;
				for (size_t i = 0; i < k.size(); ++i) {
					draw_square(k[i], half);
					draw_square(mid(k[i], k[(i + 1) % k.size()]), half);
				}
			}
		}
	}
	gs_load_vertexbuffer(nullptr); // like OBSBasicPreview::DrawSceneEditing
}

/* ---- editing (UI thread) ----------------------------------------------------- */

const preview_edit::Item *CanvasPreview::Snapshot::find(int64_t id) const
{
	if (id == 0)
		return nullptr;
	for (const auto &it : items)
		if (it.id == id)
			return &it;
	return nullptr;
}

preview_edit::Item read_preview_item(obs_sceneitem_t *item)
{
	preview_edit::Item out;
	out.id = obs_sceneitem_get_id(item);
	out.visible = obs_sceneitem_visible(item);
	out.locked = obs_sceneitem_locked(item);

	obs_transform_info info = {};
	obs_sceneitem_get_info2(item, &info); // absolute (canvas px) values
	out.pos = {info.pos.x, info.pos.y};
	out.rot = info.rot;
	out.scale = {info.scale.x, info.scale.y};
	out.alignment = info.alignment;
	out.has_bounds = info.bounds_type != OBS_BOUNDS_NONE;
	out.bounds = {info.bounds.x, info.bounds.y};

	obs_sceneitem_crop crop = {};
	obs_sceneitem_get_crop(item, &crop);
	out.crop = {crop.left, crop.top, crop.right, crop.bottom};

	obs_source_t *src = obs_sceneitem_get_source(item); // owned by the item, no ref
	const bool has_video = src && (obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO) != 0;
	if (has_video)
		out.source = {obs_source_get_width(src), obs_source_get_height(src)};
	return out;
}

namespace {

bool collect_items(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	static_cast<std::vector<preview_edit::Item> *>(param)->push_back(read_preview_item(item));
	return true;
}

struct FindById {
	int64_t id = 0;
	obs_sceneitem_t *found = nullptr;
};

/* Under the scene lock; the found item gets its own ref (caller releases). */
bool find_by_id(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *f = static_cast<FindById *>(param);
	if (obs_sceneitem_get_id(item) != f->id)
		return true;
	obs_sceneitem_addref(item);
	f->found = item;
	return false;
}

Qt::CursorShape to_qt(preview_edit::Cursor c)
{
	switch (c) {
	case preview_edit::Cursor::Move:
		return Qt::SizeAllCursor;
	case preview_edit::Cursor::SizeHor:
		return Qt::SizeHorCursor;
	case preview_edit::Cursor::SizeVer:
		return Qt::SizeVerCursor;
	case preview_edit::Cursor::SizeFDiag:
		return Qt::SizeFDiagCursor;
	case preview_edit::Cursor::SizeBDiag:
		return Qt::SizeBDiagCursor;
	default:
		return Qt::ArrowCursor;
	}
}

} // namespace

bool CanvasPreview::editing_allowed() const
{
	return !m_shutdown && static_cast<bool>(m_access);
}

bool CanvasPreview::take_snapshot(Snapshot &out) const
{
	out = Snapshot{};
	if (!editing_allowed())
		return false;
	return m_access([&out](obs_scene_t *scene, const std::string &uuid) {
		out.scene_uuid = uuid;
		obs_scene_enum_items(scene, collect_items, &out.items);
	});
}

/* Writes only position plus scale (no bounds) or bounds (with bounds); rotation, alignment and crop stay. */
bool CanvasPreview::write_item(const std::string &scene_uuid, const preview_edit::Item &next) const
{
	if (!editing_allowed() || next.id == 0)
		return false;
	bool written = false;
	m_access([&](obs_scene_t *scene, const std::string &uuid) {
		if (uuid != scene_uuid)
			return; // the active scene changed: the id would mean another item
		FindById f;
		f.id = next.id;
		obs_scene_enum_items(scene, find_by_id, &f);
		if (!f.found)
			return;
		if (!obs_sceneitem_locked(f.found)) {
			vec2 pos;
			vec2_set(&pos, next.pos.x, next.pos.y);
			obs_sceneitem_defer_update_begin(f.found);
			obs_sceneitem_set_pos(f.found, &pos);
			if (next.has_bounds) {
				vec2 bounds;
				vec2_set(&bounds, next.bounds.x, next.bounds.y);
				obs_sceneitem_set_bounds(f.found, &bounds);
			} else {
				vec2 scale;
				vec2_set(&scale, next.scale.x, next.scale.y);
				obs_sceneitem_set_scale(f.found, &scale);
			}
			obs_sceneitem_defer_update_end(f.found);
			written = true;
		}
		obs_sceneitem_release(f.found);
	});
	return written;
}

layout::Size CanvasPreview::canvas_size() const
{
	layout::Size s{kFallbackWidth, kFallbackHeight};
	obs_canvas_t *canvas = m_canvas.get_ref();
	if (!canvas)
		return s;
	obs_video_info ovi = {};
	if (obs_canvas_has_video(canvas) && obs_canvas_get_video_info(canvas, &ovi) && ovi.base_width > 0 &&
	    ovi.base_height > 0)
		s = {ovi.base_width, ovi.base_height};
	obs_canvas_release(canvas); // UI thread
	return s;
}

preview_edit::View CanvasPreview::current_view() const
{
	const QSize px = pixel_size(this);
	return preview_edit::make_view(canvas_size(), {static_cast<uint32_t>(std::max(0, px.width())),
						       static_cast<uint32_t>(std::max(0, px.height()))});
}

preview_edit::Vec2 CanvasPreview::event_pos(const QMouseEvent *event) const
{
	const QPointF p = event->position() * devicePixelRatioF();
	return {static_cast<float>(p.x()), static_cast<float>(p.y())};
}

void CanvasPreview::set_selected_overlay(const preview_edit::Item *item)
{
	if (item && !item->visible)
		item = nullptr; // OBS does not outline hidden items
	const float dpr = static_cast<float>(devicePixelRatioF());
	std::lock_guard<std::mutex> lock(m_overlay_mutex);
	m_overlay.dpr = dpr;
	m_overlay.selected = item != nullptr;
	if (item) {
		m_overlay.selected_locked = item->locked;
		m_overlay.selected_corners = preview_edit::corners(*item);
	}
}

void CanvasPreview::refresh_overlay()
{
	Snapshot snap;
	const bool ok = take_snapshot(snap);
	const preview_edit::Item *sel = ok ? snap.find(m_selection.item_in(snap.scene_uuid)) : nullptr;
	const preview_edit::Item *hover = ok ? snap.find(m_hover_id) : nullptr;
	if (hover && (hover == sel || !hover->visible || hover->locked))
		hover = nullptr;
	if (m_drag.mode != DragMode::None && sel)
		return; // the drag owns the selected geometry until release
	if (sel && !sel->visible)
		sel = nullptr; // stays selected (list, panel), but OBS does not outline hidden items
	const float dpr = static_cast<float>(devicePixelRatioF());
	std::lock_guard<std::mutex> lock(m_overlay_mutex);
	m_overlay.dpr = dpr;
	m_overlay.selected = sel != nullptr;
	if (sel) {
		m_overlay.selected_locked = sel->locked;
		m_overlay.selected_corners = preview_edit::corners(*sel);
	}
	m_overlay.hovered = hover != nullptr;
	if (hover)
		m_overlay.hovered_corners = preview_edit::corners(*hover);
}

void CanvasPreview::notify_edit()
{
	if (m_edit_listener && !m_shutdown)
		m_edit_listener();
}

bool CanvasPreview::event(QEvent *event)
{
	/* OBS binds Delete, Ctrl+Z and arrows to main-canvas actions in the main window. Claim them while the
	 * preview has focus so they reach keyPressEvent instead (Delete then does nothing, by design). */
	if (event->type() == QEvent::ShortcutOverride && editing_allowed()) {
		auto *ke = static_cast<QKeyEvent *>(event);
		const int key = ke->key();
		const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;
		/* Ctrl+Z = our undo; Ctrl+Y and Ctrl+Shift+Z (OBS main redo) are claimed as no-ops. */
		const bool ctrl_z = key == Qt::Key_Z &&
				    (mods == Qt::ControlModifier || mods == (Qt::ControlModifier | Qt::ShiftModifier));
		const bool ctrl_y = key == Qt::Key_Y && mods == Qt::ControlModifier;
		if (ctrl_z || ctrl_y || key == Qt::Key_Escape || key == Qt::Key_Delete || key == Qt::Key_Backspace ||
		    key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down) {
			ke->accept();
			return true;
		}
	}
	return QWidget::event(event);
}

/* Right click: select the item under the cursor (the selected one wins inside its box, like a left click)
 * and hand it to the dock's item menu. Nothing is kept: the handler resolves the item by id when it acts. */
void CanvasPreview::contextMenuEvent(QContextMenuEvent *event)
{
	if (!editing_allowed() || !m_context_handler || m_drag.mode != DragMode::None) {
		QWidget::contextMenuEvent(event);
		return;
	}
	Snapshot snap;
	if (!take_snapshot(snap))
		return;
	const preview_edit::Vec2 cp = preview_edit::widget_to_canvas(
		current_view(),
		{static_cast<float>(event->pos().x() * devicePixelRatioF()),
		 static_cast<float>(event->pos().y() * devicePixelRatioF())});
	const preview_edit::Item *sel = snap.find(m_selection.item_in(snap.scene_uuid));
	int64_t hit = 0;
	if (sel && !sel->locked && sel->visible && preview_edit::hit_item({*sel}, cp) == sel->id)
		hit = sel->id;
	if (hit == 0)
		hit = preview_edit::hit_item(snap.items, cp);
	if (hit == 0) {
		event->accept();
		return;
	}
	m_selection.set(snap.scene_uuid, hit);
	event->accept();
	m_context_handler(event->globalPos(), hit);
}

void CanvasPreview::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || !editing_allowed()) {
		QWidget::mousePressEvent(event);
		return;
	}
	setFocus(Qt::MouseFocusReason);
	if (m_drag.mode != DragMode::None)
		return;

	Snapshot snap;
	if (!take_snapshot(snap)) {
		m_selection.clear();
		refresh_overlay();
		return;
	}
	const preview_edit::View view = current_view();
	const preview_edit::Vec2 wp = event_pos(event);
	const preview_edit::Vec2 cp = preview_edit::widget_to_canvas(view, wp);
	const float dpr = static_cast<float>(devicePixelRatioF());

	Drag drag;
	drag.scene_uuid = snap.scene_uuid;
	drag.press_canvas = cp;
	drag.snap_canvas = preview_edit::widget_to_canvas_length(view, preview_edit::kSnapThresholdPx * dpr);

	/* Handles of the selected item first (they reach outside its box), then the topmost item. */
	const preview_edit::Item *sel = snap.find(m_selection.item_in(snap.scene_uuid));
	if (sel && !sel->locked && sel->visible) {
		const preview_edit::Handle h =
			preview_edit::hit_handle(*sel, view, wp, preview_edit::kHandleRadiusPx * dpr);
		if (h != preview_edit::Handle::None) {
			drag.mode = DragMode::Resize;
			drag.handle = h;
			drag.handle_start = preview_edit::box_point(*sel, preview_edit::handle_unit(h));
			drag.id = sel->id;
			drag.start = *sel;
		}
	}
	if (drag.mode == DragMode::None) {
		/* Like OBS: a click inside the selected item keeps dragging it even when another item lies on top. */
		int64_t hit = 0;
		if (sel && !sel->locked && sel->visible && preview_edit::hit_item({*sel}, cp) == sel->id)
			hit = sel->id;
		if (hit == 0)
			hit = preview_edit::hit_item(snap.items, cp);
		if (hit == 0) {
			m_selection.clear();
			refresh_overlay();
			return;
		}
		m_selection.set(snap.scene_uuid, hit);
		drag.mode = DragMode::Move;
		drag.id = hit;
		drag.start = *snap.find(hit);
	}
	for (const auto &it : snap.items)
		if (it.id != drag.id)
			drag.others.push_back(it);
	m_drag = std::move(drag);
	preview_edit::end_nudge_burst(m_nudge_burst);
	set_selected_overlay(&m_drag.start);
}

void CanvasPreview::mouseMoveEvent(QMouseEvent *event)
{
	if (!editing_allowed()) {
		QWidget::mouseMoveEvent(event);
		return;
	}
	const preview_edit::Vec2 wp = event_pos(event);
	if (m_drag.mode == DragMode::None) {
		update_hover(wp);
		return;
	}
	if (!preview_edit::drag_continues((event->buttons() & Qt::LeftButton) != 0)) {
		/* The release went elsewhere (Alt+Tab, a popup took the grab): end the drag, never move on hover. */
		finish_drag(false);
		update_hover(wp);
		return;
	}

	const preview_edit::View view = current_view();
	const preview_edit::Vec2 delta = preview_edit::widget_to_canvas(view, wp) - m_drag.press_canvas;
	const bool ctrl = (event->modifiers() & Qt::ControlModifier) != 0;
	const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;

	preview_edit::Item next = m_drag.start;
	if (m_drag.mode == DragMode::Move)
		next.pos = preview_edit::move_position(m_drag.start, delta, canvas_size(), m_drag.others,
						       m_drag.snap_canvas, ctrl);
	else
		next = preview_edit::resize(m_drag.start, m_drag.handle, m_drag.handle_start + delta, shift);

	if (!write_item(m_drag.scene_uuid, next)) {
		/* Item, scene or canvas gone (or locked meanwhile): end the drag, nothing to restore. */
		m_drag = Drag{};
		refresh_overlay();
		return;
	}
	m_drag.changed = true;
	set_selected_overlay(&next);
}

void CanvasPreview::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || m_drag.mode == DragMode::None) {
		QWidget::mouseReleaseEvent(event);
		return;
	}
	finish_drag(false);
}

/* restore: Escape, put the transform captured at press back. Otherwise keep it and remember it for undo. */
void CanvasPreview::finish_drag(bool restore)
{
	if (m_drag.mode == DragMode::None)
		return;
	const Drag drag = std::move(m_drag);
	m_drag = Drag{};
	if (drag.changed) {
		if (restore) {
			write_item(drag.scene_uuid, drag.start);
		} else {
			m_undo.valid = true;
			m_undo.scene_uuid = drag.scene_uuid;
			m_undo.id = drag.id;
			m_undo.before = drag.start;
		}
	}
	refresh_overlay();
	notify_edit();
}

void CanvasPreview::nudge_selected(int dx, int dy, bool shift)
{
	Snapshot snap;
	if (!take_snapshot(snap))
		return;
	const preview_edit::Item *sel = snap.find(m_selection.item_in(snap.scene_uuid));
	if (!sel || sel->locked)
		return;
	preview_edit::Item next = *sel;
	next.pos = preview_edit::nudge(sel->pos, dx, dy, shift);
	if (!write_item(snap.scene_uuid, next)) {
		preview_edit::end_nudge_burst(m_nudge_burst);
		return;
	}
	/* One undo step per burst: capture the transform before the first nudge only. */
	if (preview_edit::nudge_begins_step(m_nudge_burst, snap.scene_uuid, sel->id, m_clock.elapsed()) ||
	    !m_undo.valid) {
		m_undo.valid = true;
		m_undo.scene_uuid = snap.scene_uuid;
		m_undo.id = sel->id;
		m_undo.before = *sel;
	}
	refresh_overlay();
	notify_edit();
}

void CanvasPreview::undo_last()
{
	if (!m_undo.valid)
		return;
	const Undo undo = m_undo;
	m_undo = Undo{};
	preview_edit::end_nudge_burst(m_nudge_burst);
	write_item(undo.scene_uuid, undo.before); // refused when the scene changed or the item is gone
	refresh_overlay();
	notify_edit();
}

void CanvasPreview::keyPressEvent(QKeyEvent *event)
{
	if (!editing_allowed()) {
		QWidget::keyPressEvent(event);
		return;
	}
	const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;
	switch (event->key()) {
	case Qt::Key_Escape:
		finish_drag(true);
		break;
	case Qt::Key_Z:
	case Qt::Key_Y: {
		const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
		const bool undo = event->key() == Qt::Key_Z && mods == Qt::ControlModifier;
		const bool redo = (event->key() == Qt::Key_Y && mods == Qt::ControlModifier) ||
				  (event->key() == Qt::Key_Z && mods == (Qt::ControlModifier | Qt::ShiftModifier));
		if (!undo && !redo) {
			QWidget::keyPressEvent(event);
			return;
		}
		if (undo && m_drag.mode == DragMode::None)
			undo_last();
		break; // redo: no-op here, so OBS's main-canvas redo never fires from the preview
	}
	case Qt::Key_Left:
	case Qt::Key_Right:
	case Qt::Key_Up:
	case Qt::Key_Down:
		if (m_drag.mode == DragMode::None) {
			const int dx = event->key() == Qt::Key_Left ? -1 : event->key() == Qt::Key_Right ? 1 : 0;
			const int dy = event->key() == Qt::Key_Up ? -1 : event->key() == Qt::Key_Down ? 1 : 0;
			nudge_selected(dx, dy, shift);
		}
		break;
	case Qt::Key_Delete:
	case Qt::Key_Backspace:
		break; // no destructive shortcut in the preview
	default:
		QWidget::keyPressEvent(event);
		return;
	}
	event->accept();
}

void CanvasPreview::update_hover(const preview_edit::Vec2 &widget_px)
{
	Snapshot snap;
	if (!take_snapshot(snap)) {
		m_hover_id = 0;
		unsetCursor();
		refresh_overlay();
		return;
	}
	const preview_edit::View view = current_view();
	const float dpr = static_cast<float>(devicePixelRatioF());
	const preview_edit::Item *sel = snap.find(m_selection.item_in(snap.scene_uuid));

	preview_edit::Cursor cursor = preview_edit::Cursor::Arrow;
	if (sel && !sel->locked && sel->visible) {
		const preview_edit::Handle h =
			preview_edit::hit_handle(*sel, view, widget_px, preview_edit::kHandleRadiusPx * dpr);
		if (h != preview_edit::Handle::None)
			cursor = preview_edit::handle_cursor(h, sel->rot);
	}
	const int64_t hit = preview_edit::hit_item(snap.items, preview_edit::widget_to_canvas(view, widget_px));
	if (cursor == preview_edit::Cursor::Arrow && hit != 0)
		cursor = preview_edit::Cursor::Move;
	setCursor(to_qt(cursor));

	if (hit != m_hover_id) {
		m_hover_id = hit;
		refresh_overlay();
	}
}

void CanvasPreview::focusOutEvent(QFocusEvent *event)
{
	QWidget::focusOutEvent(event);
	finish_drag(false); // a lost focus usually means the release goes elsewhere
}

void CanvasPreview::leaveEvent(QEvent *event)
{
	QWidget::leaveEvent(event);
	if (m_hover_id != 0) {
		m_hover_id = 0;
		refresh_overlay();
	}
}

} // namespace smulti
