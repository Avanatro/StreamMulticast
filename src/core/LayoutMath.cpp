/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "LayoutMath.hpp"

#include <algorithm>
#include <cmath>

namespace smulti::layout {

namespace {

bool degenerate(Size source, Size canvas)
{
	return source.w == 0 || source.h == 0 || canvas.w == 0 || canvas.h == 0;
}

} // namespace

ItemTransform fit(Size source, Size canvas)
{
	ItemTransform t;
	if (degenerate(source, canvas))
		return t;

	const float sx = static_cast<float>(canvas.w) / static_cast<float>(source.w);
	const float sy = static_cast<float>(canvas.h) / static_cast<float>(source.h);
	const float s = std::min(sx, sy);
	t.scale_x = s;
	t.scale_y = s;
	t.pos_x = (static_cast<float>(canvas.w) - static_cast<float>(source.w) * s) / 2.0f;
	t.pos_y = (static_cast<float>(canvas.h) - static_cast<float>(source.h) * s) / 2.0f;
	return t;
}

ItemTransform fill(Size source, Size canvas)
{
	ItemTransform t;
	if (degenerate(source, canvas))
		return t;

	const float sx = static_cast<float>(canvas.w) / static_cast<float>(source.w);
	const float sy = static_cast<float>(canvas.h) / static_cast<float>(source.h);
	const float s = std::max(sx, sy);
	t.scale_x = s;
	t.scale_y = s;

	// Source pixels that stay visible on the canvas; the rest is cropped evenly.
	const float visible_w = static_cast<float>(canvas.w) / s;
	const float visible_h = static_cast<float>(canvas.h) / s;
	const int over_w = static_cast<int>(std::lround((static_cast<float>(source.w) - visible_w) / 2.0f));
	const int over_h = static_cast<int>(std::lround((static_cast<float>(source.h) - visible_h) / 2.0f));
	Crop c;
	c.left = c.right = std::max(0, over_w);
	c.top = c.bottom = std::max(0, over_h);
	t.crop = clamp_crop(c, source);
	return t;
}

Crop clamp_crop(Crop c, Size source)
{
	Crop out;
	const int max_w = source.w > 0 ? static_cast<int>(std::min<uint32_t>(source.w, 0x7fffffffu)) - 1 : 0;
	const int max_h = source.h > 0 ? static_cast<int>(std::min<uint32_t>(source.h, 0x7fffffffu)) - 1 : 0;

	out.left = std::clamp(c.left, 0, max_w);
	out.right = std::clamp(c.right, 0, max_w - out.left);
	out.top = std::clamp(c.top, 0, max_h);
	out.bottom = std::clamp(c.bottom, 0, max_h - out.top);
	return out;
}

Rect letterbox(Size canvas, Size widget)
{
	Rect r;
	if (degenerate(canvas, widget))
		return r;
	const ItemTransform t = fit(canvas, widget);
	r.x = static_cast<int>(std::lround(t.pos_x));
	r.y = static_cast<int>(std::lround(t.pos_y));
	r.w = static_cast<int>(std::lround(static_cast<float>(canvas.w) * t.scale_x));
	r.h = static_cast<int>(std::lround(static_cast<float>(canvas.h) * t.scale_y));
	return r;
}

} // namespace smulti::layout
