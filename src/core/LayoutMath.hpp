/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

#include <cstdint>

namespace smulti::layout {

struct Size {
	uint32_t w = 0;
	uint32_t h = 0;
};

/// Crop in source pixels, same meaning as obs_sceneitem_crop.
struct Crop {
	int left = 0, top = 0, right = 0, bottom = 0;
};

struct ItemTransform {
	float pos_x = 0, pos_y = 0, scale_x = 1, scale_y = 1;
	Crop crop;
};

/// Uniform scale min(cw/sw, ch/sh), centred on the canvas, no crop.
/// A zero-sized source or canvas yields the identity transform.
ItemTransform fit(Size source, Size canvas);

/// Uniform scale max(cw/sw, ch/sh), pos (0,0); a symmetric crop (in source
/// pixels) removes the overflow so the visible part equals the canvas.
/// A zero-sized source or canvas yields the identity transform.
ItemTransform fill(Size source, Size canvas);

/// Each value >= 0, left+right <= w-1 and top+bottom <= h-1 (at least one
/// source pixel stays visible). A zero-sized source clamps everything to 0.
Crop clamp_crop(Crop c, Size source);

} // namespace smulti::layout
