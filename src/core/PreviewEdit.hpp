/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include "LayoutMath.hpp"

#include <array>
#include <cstdint>
#include <vector>

/**
 * Pure math for click-and-drag editing in the vertical dock preview (OBS-free, Qt-free).
 *
 * Coordinates: "canvas" = canvas pixels (1080x1920 for the owned canvas); "widget" = the preview's
 * pixels as the render uses them (device pixels). Geometry follows libobs update_item_transform:
 *   canvas = pos + R(rot) * (unit * box_size - alignment_origin(box_size)),
 * with box_size = bounds when the item has a bounding box, else scale * (source - crop), and R the
 * libobs rotation (degrees, clockwise on the y-down canvas).
 */
namespace smulti::preview_edit {

struct Vec2 {
	float x = 0, y = 0;
};
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }

constexpr float kHandleRadiusPx = 6.0f;   ///< handle hit radius, widget px
constexpr float kSnapThresholdPx = 8.0f;  ///< snap distance, widget px (convert with widget_to_canvas_length)
constexpr float kMinSizeCanvas = 8.0f;    ///< smallest box side a resize produces, canvas px
constexpr float kNudgeStep = 1.0f;        ///< arrow key, canvas px
constexpr float kNudgeStepShift = 10.0f;  ///< Shift+arrow, canvas px

/** Same bits as OBS_ALIGN_* (0 = centre). */
enum Align : uint32_t { AlignLeft = 1u << 0, AlignRight = 1u << 1, AlignTop = 1u << 2, AlignBottom = 1u << 3 };

/** widget = origin + canvas * scale (per axis), from the rounded letterbox viewport. */
struct View {
	float x = 0, y = 0;
	float sx = 1, sy = 1;
};

View make_view(layout::Size canvas, layout::Size widget);
Vec2 canvas_to_widget(const View &v, Vec2 canvas);
Vec2 widget_to_canvas(const View &v, Vec2 widget);
/** A widget-pixel distance in canvas pixels (mean of both axis scales). */
float widget_to_canvas_length(const View &v, float widget_px);

/** One scene item as the editor sees it; filled from obs_sceneitem_get_info2/get_crop/source size. */
struct Item {
	int64_t id = 0;
	bool visible = true;
	bool locked = false;
	Vec2 pos;
	float rot = 0;        ///< degrees
	Vec2 scale{1, 1};
	uint32_t alignment = AlignLeft | AlignTop;
	bool has_bounds = false; ///< bounds_type != OBS_BOUNDS_NONE
	Vec2 bounds;
	layout::Crop crop;
	layout::Size source;
};

/** Box size in canvas px (signed: a negative scale flips). */
Vec2 box_size(const Item &it);
/** unit (0..1 across the box) -> canvas point. */
Vec2 box_point(const Item &it, Vec2 unit);
/** canvas point -> unit; false for a degenerate (zero-sized) box. */
bool canvas_to_unit(const Item &it, Vec2 canvas, Vec2 &unit);
/** top-left, top-right, bottom-right, bottom-left (item-local naming). */
std::array<Vec2, 4> corners(const Item &it);

/** Topmost visible, unlocked, non-degenerate item containing pt; items in libobs order (bottom first). 0 = none. */
int64_t hit_item(const std::vector<Item> &bottom_to_top, Vec2 canvas_pt);

/** Handle bits: a corner has one x and one y bit, an edge one bit. */
enum class Handle : uint32_t {
	None = 0,
	Left = 1,
	Right = 2,
	Top = 4,
	Bottom = 8,
	TopLeft = 4 | 1,
	TopRight = 4 | 2,
	BottomLeft = 8 | 1,
	BottomRight = 8 | 2,
};

/** Unit position of a handle on the box (edges at the midpoints). */
Vec2 handle_unit(Handle h);
bool is_corner(Handle h);
/** Nearest of the 8 handles within radius_px (widget px) of widget_pt, else None. */
Handle hit_handle(const Item &it, const View &v, Vec2 widget_pt, float radius_px = kHandleRadiusPx);

/**
 * New position for a drag by delta (canvas px). Unless ctrl is held, the moved box's axis-aligned
 * extent snaps (per axis, nearest within threshold_canvas) its edges to the canvas edges and to the
 * edges of the visible `others`, and its centre to the canvas centre lines.
 */
Vec2 move_position(const Item &start, Vec2 delta, layout::Size canvas, const std::vector<Item> &others,
		   float threshold_canvas, bool ctrl);

/**
 * Resize by dragging handle h to target (canvas px). The opposite corner/edge stays fixed. Corners keep
 * the aspect ratio unless shift; edges change one axis. Items with bounds change bounds, others change
 * scale. Each side stays >= kMinSizeCanvas and never flips. Works for any rotation (the drag is
 * projected onto the item's own axes). A degenerate box or Handle::None returns start unchanged.
 */
Item resize(const Item &start, Handle h, Vec2 target, bool shift);

/** Arrow-key nudge: dx/dy in {-1,0,1}, 1 canvas px, 10 with shift. */
Vec2 nudge(Vec2 pos, int dx, int dy, bool shift);

enum class Cursor { Arrow, Move, SizeHor, SizeVer, SizeFDiag, SizeBDiag };
/** Resize cursor for a handle on an item rotated by rot degrees (Move for None). */
Cursor handle_cursor(Handle h, float rot);

} // namespace smulti::preview_edit
