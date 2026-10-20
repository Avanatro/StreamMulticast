/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "PreviewEdit.hpp"

#include <algorithm>
#include <cmath>

namespace smulti::preview_edit {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTiny = 1e-6f;

/* Same convention as libobs matrix4_rotate_aa4f about +z and OBSBasicPreview RotatePos. */
Vec2 rotate(Vec2 p, float deg)
{
	const float r = deg * kPi / 180.0f;
	const float c = std::cos(r);
	const float s = std::sin(r);
	return {c * p.x - s * p.y, s * p.x + c * p.y};
}

/* libobs add_alignment: the box size is truncated to int, centre uses integer halving. */
Vec2 alignment_origin(uint32_t align, Vec2 size)
{
	const int cx = static_cast<int>(size.x);
	const int cy = static_cast<int>(size.y);
	Vec2 o;
	if (align & AlignRight)
		o.x = static_cast<float>(cx);
	else if ((align & AlignLeft) == 0)
		o.x = static_cast<float>(cx / 2);
	if (align & AlignBottom)
		o.y = static_cast<float>(cy);
	else if ((align & AlignTop) == 0)
		o.y = static_cast<float>(cy / 2);
	return o;
}

bool degenerate(Vec2 size)
{
	return std::fabs(size.x) < kTiny || std::fabs(size.y) < kTiny;
}

struct Extent {
	float min_x, max_x, min_y, max_y;
};

Extent extent(const Item &it)
{
	const auto c = corners(it);
	Extent e{c[0].x, c[0].x, c[0].y, c[0].y};
	for (const Vec2 &p : c) {
		e.min_x = std::min(e.min_x, p.x);
		e.max_x = std::max(e.max_x, p.x);
		e.min_y = std::min(e.min_y, p.y);
		e.max_y = std::max(e.max_y, p.y);
	}
	return e;
}

/* Smallest offset that moves one of `probes` onto one of `targets` within threshold; 0 if none. */
float best_snap(std::initializer_list<float> probes, const std::vector<float> &targets, float threshold, bool &found)
{
	float best = 0.0f;
	float best_abs = threshold;
	found = false;
	for (float p : probes) {
		for (float t : targets) {
			const float off = t - p;
			if (std::fabs(off) <= best_abs) {
				best_abs = std::fabs(off);
				best = off;
				found = true;
			}
		}
	}
	return best;
}

/* Keep the sign of the original side (no flip) and at least kMinSizeCanvas. */
float clamp_side(float v, float original)
{
	const float sign = original < 0.0f ? -1.0f : 1.0f;
	return std::max(v * sign, kMinSizeCanvas) * sign;
}

} // namespace

/* ---- view mapping ---------------------------------------------------------- */

View make_view(layout::Size canvas, layout::Size widget)
{
	View v;
	const layout::Rect r = layout::letterbox(canvas, widget);
	if (r.w <= 0 || r.h <= 0 || canvas.w == 0 || canvas.h == 0)
		return v; // identity: keeps the mapping finite for a not yet sized widget
	v.x = static_cast<float>(r.x);
	v.y = static_cast<float>(r.y);
	v.sx = static_cast<float>(r.w) / static_cast<float>(canvas.w);
	v.sy = static_cast<float>(r.h) / static_cast<float>(canvas.h);
	return v;
}

Vec2 canvas_to_widget(const View &v, Vec2 c)
{
	return {v.x + c.x * v.sx, v.y + c.y * v.sy};
}

Vec2 widget_to_canvas(const View &v, Vec2 w)
{
	return {(w.x - v.x) / v.sx, (w.y - v.y) / v.sy};
}

float widget_to_canvas_length(const View &v, float widget_px)
{
	return widget_px / ((v.sx + v.sy) * 0.5f);
}

/* ---- geometry -------------------------------------------------------------- */

Vec2 box_size(const Item &it)
{
	if (it.has_bounds)
		return it.bounds;
	const int w = static_cast<int>(std::min<uint32_t>(it.source.w, 0x7fffffffu)) - it.crop.left - it.crop.right;
	const int h = static_cast<int>(std::min<uint32_t>(it.source.h, 0x7fffffffu)) - it.crop.top - it.crop.bottom;
	return {it.scale.x * static_cast<float>(std::max(0, w)), it.scale.y * static_cast<float>(std::max(0, h))};
}

Vec2 box_point(const Item &it, Vec2 unit)
{
	const Vec2 size = box_size(it);
	const Vec2 origin = alignment_origin(it.alignment, size);
	return it.pos + rotate({unit.x * size.x - origin.x, unit.y * size.y - origin.y}, it.rot);
}

bool canvas_to_unit(const Item &it, Vec2 canvas, Vec2 &unit)
{
	const Vec2 size = box_size(it);
	if (degenerate(size))
		return false;
	const Vec2 origin = alignment_origin(it.alignment, size);
	const Vec2 local = rotate(canvas - it.pos, -it.rot) + origin;
	unit = {local.x / size.x, local.y / size.y};
	return true;
}

std::array<Vec2, 4> corners(const Item &it)
{
	return {box_point(it, {0, 0}), box_point(it, {1, 0}), box_point(it, {1, 1}), box_point(it, {0, 1})};
}

namespace {
int64_t hit_topmost(const std::vector<Item> &bottom_to_top, Vec2 canvas_pt, bool include_locked)
{
	for (auto it = bottom_to_top.rbegin(); it != bottom_to_top.rend(); ++it) {
		if (!it->visible || (it->locked && !include_locked))
			continue;
		Vec2 u;
		if (!canvas_to_unit(*it, canvas_pt, u))
			continue;
		if (u.x >= 0.0f && u.x <= 1.0f && u.y >= 0.0f && u.y <= 1.0f)
			return it->id;
	}
	return 0;
}
} // namespace

int64_t hit_item(const std::vector<Item> &bottom_to_top, Vec2 canvas_pt)
{
	return hit_topmost(bottom_to_top, canvas_pt, false);
}

int64_t hit_item_selectable(const std::vector<Item> &bottom_to_top, Vec2 canvas_pt)
{
	return hit_topmost(bottom_to_top, canvas_pt, true);
}

/* ---- handles --------------------------------------------------------------- */

Vec2 handle_unit(Handle h)
{
	const uint32_t b = static_cast<uint32_t>(h);
	Vec2 u{0.5f, 0.5f};
	if (b & static_cast<uint32_t>(Handle::Left))
		u.x = 0.0f;
	else if (b & static_cast<uint32_t>(Handle::Right))
		u.x = 1.0f;
	if (b & static_cast<uint32_t>(Handle::Top))
		u.y = 0.0f;
	else if (b & static_cast<uint32_t>(Handle::Bottom))
		u.y = 1.0f;
	return u;
}

bool is_corner(Handle h)
{
	const uint32_t b = static_cast<uint32_t>(h);
	const bool x = (b & (static_cast<uint32_t>(Handle::Left) | static_cast<uint32_t>(Handle::Right))) != 0;
	const bool y = (b & (static_cast<uint32_t>(Handle::Top) | static_cast<uint32_t>(Handle::Bottom))) != 0;
	return x && y;
}

Handle hit_handle(const Item &it, const View &v, Vec2 widget_pt, float radius_px)
{
	static constexpr Handle kAll[] = {Handle::TopLeft,     Handle::Top,    Handle::TopRight,   Handle::Right,
					  Handle::BottomRight, Handle::Bottom, Handle::BottomLeft, Handle::Left};
	if (degenerate(box_size(it)))
		return Handle::None;
	Handle best = Handle::None;
	float best_d = radius_px;
	for (Handle h : kAll) {
		const Vec2 w = canvas_to_widget(v, box_point(it, handle_unit(h)));
		const float d = std::hypot(w.x - widget_pt.x, w.y - widget_pt.y);
		if (d <= best_d) {
			best_d = d;
			best = h;
		}
	}
	return best;
}

/* ---- move ------------------------------------------------------------------ */

Vec2 move_position(const Item &start, Vec2 delta, layout::Size canvas, const std::vector<Item> &others,
		   float threshold_canvas, bool ctrl)
{
	Item moved = start;
	moved.pos = start.pos + delta;
	if (ctrl || threshold_canvas <= 0.0f || degenerate(box_size(moved)))
		return moved.pos;

	const float cw = static_cast<float>(canvas.w);
	const float ch = static_cast<float>(canvas.h);
	std::vector<float> xs{0.0f, cw};
	std::vector<float> ys{0.0f, ch};
	for (const Item &o : others) {
		if (!o.visible || o.id == start.id || degenerate(box_size(o)))
			continue;
		const Extent e = extent(o);
		xs.insert(xs.end(), {e.min_x, e.max_x});
		ys.insert(ys.end(), {e.min_y, e.max_y});
	}

	const Extent e = extent(moved);
	const float mid_x = (e.min_x + e.max_x) * 0.5f;
	const float mid_y = (e.min_y + e.max_y) * 0.5f;

	bool edge_x = false, centre_x = false, edge_y = false, centre_y = false;
	const float off_edge_x = best_snap({e.min_x, e.max_x}, xs, threshold_canvas, edge_x);
	const float off_centre_x = best_snap({mid_x}, {cw * 0.5f}, threshold_canvas, centre_x);
	const float off_edge_y = best_snap({e.min_y, e.max_y}, ys, threshold_canvas, edge_y);
	const float off_centre_y = best_snap({mid_y}, {ch * 0.5f}, threshold_canvas, centre_y);

	auto pick = [](bool a, float oa, bool b, float ob) {
		if (a && b)
			return std::fabs(oa) <= std::fabs(ob) ? oa : ob;
		return a ? oa : (b ? ob : 0.0f);
	};
	moved.pos.x += pick(edge_x, off_edge_x, centre_x, off_centre_x);
	moved.pos.y += pick(edge_y, off_edge_y, centre_y, off_centre_y);
	return moved.pos;
}

/* ---- resize ---------------------------------------------------------------- */

Item resize(const Item &start, Handle h, Vec2 target, bool shift)
{
	const Vec2 size = box_size(start);
	if (h == Handle::None || degenerate(size))
		return start;

	const Vec2 hu = handle_unit(h);
	const Vec2 au{1.0f - hu.x, 1.0f - hu.y}; // opposite corner / edge midpoint
	const Vec2 anchor = box_point(start, au);
	const Vec2 local = rotate(target - anchor, -start.rot); // drag in the item's own axes

	Vec2 next = size;
	const bool moves_x = hu.x != au.x;
	const bool moves_y = hu.y != au.y;
	if (moves_x)
		next.x = local.x / (hu.x - au.x);
	if (moves_y)
		next.y = local.y / (hu.y - au.y);

	if (is_corner(h) && !shift) {
		const float fx = next.x / size.x;
		const float fy = next.y / size.y;
		float f = std::fabs(fx) >= std::fabs(fy) ? fx : fy;
		const float f_min = std::max(kMinSizeCanvas / std::fabs(size.x), kMinSizeCanvas / std::fabs(size.y));
		f = std::max(f, f_min);
		next = {size.x * f, size.y * f};
	} else {
		if (moves_x)
			next.x = clamp_side(next.x, size.x);
		if (moves_y)
			next.y = clamp_side(next.y, size.y);
	}

	Item out = start;
	if (start.has_bounds) {
		out.bounds = next;
	} else {
		out.scale.x = start.scale.x * (next.x / size.x);
		out.scale.y = start.scale.y * (next.y / size.y);
	}

	/* Put the anchor back where it was: pos = anchor - R * (au * size' - origin'). */
	const Vec2 new_size = box_size(out);
	const Vec2 origin = alignment_origin(out.alignment, new_size);
	out.pos = anchor - rotate({au.x * new_size.x - origin.x, au.y * new_size.y - origin.y}, out.rot);
	return out;
}

/* ---- keyboard -------------------------------------------------------------- */

Vec2 nudge(Vec2 pos, int dx, int dy, bool shift)
{
	const float step = shift ? kNudgeStepShift : kNudgeStep;
	return {pos.x + static_cast<float>(dx) * step, pos.y + static_cast<float>(dy) * step};
}

bool nudge_begins_step(NudgeBurst &burst, const std::string &scene_uuid, int64_t id, int64_t now_ms)
{
	const int64_t idle = now_ms - burst.last_ms;
	const bool same = burst.active && burst.scene_uuid == scene_uuid && burst.id == id && idle >= 0 &&
			  idle < kNudgeBurstIdleMs;
	burst.active = true;
	burst.scene_uuid = scene_uuid;
	burst.id = id;
	burst.last_ms = now_ms;
	return !same;
}

void end_nudge_burst(NudgeBurst &burst)
{
	burst = NudgeBurst{};
}

Cursor handle_cursor(Handle h, float rot)
{
	if (h == Handle::None)
		return Cursor::Move;
	/* Direction of the resize axis on a y-down canvas: horizontal 0, "\" 45, vertical 90, "/" 135. */
	float base = 0.0f;
	switch (h) {
	case Handle::Left:
	case Handle::Right:
		base = 0.0f;
		break;
	case Handle::Top:
	case Handle::Bottom:
		base = 90.0f;
		break;
	case Handle::TopLeft:
	case Handle::BottomRight:
		base = 45.0f;
		break;
	default: // TopRight, BottomLeft
		base = 135.0f;
		break;
	}
	float a = std::fmod(base + rot, 180.0f);
	if (a < 0.0f)
		a += 180.0f;
	const int step = static_cast<int>(std::lround(a / 45.0f)) % 4;
	switch (step) {
	case 0:
		return Cursor::SizeHor;
	case 1:
		return Cursor::SizeFDiag;
	case 2:
		return Cursor::SizeVer;
	default:
		return Cursor::SizeBDiag;
	}
}

} // namespace smulti::preview_edit
